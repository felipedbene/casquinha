# Backend Exhaustion Audit — Open Transport sync ops (Fase 1)

Data: 2026-07-05 · Escopo: cliente Casquinha (OS 9/OT) exaurindo o gopher-spot
(geomyidae + dcgi, K8s, porta 70). **Fase 1 = só diagnóstico.** A Fase 2
(fios A–H abaixo; F–H vieram da avaliação do CLIENTS.md do backend) aguarda OK
antes de qualquer implementação.

## TLDR

A disciplina non-blocking que temíamos estar pendente **já está implementada** —
o endpoint OT é aberto `sync + non-blocking` e bombeado do loop
(`src/cq_transport_ot.c:189-191`). O suspeito nº 1 de exaustão é outro:
**retry storm no fetch de capa** — falha de transporte no `/cover` não marca o
álbum como "tentado", então o cliente refaz a conexão a cada passada do loop,
indefinidamente (`os9/casquinha.c:466-478`). Somam-se: até **7 conexões TCP
simultâneas** de um único cliente, polling que **continua em background**
(nenhum tratamento de `osEvt`/suspend), e close do cliente que **nunca completa
o orderly release** no caminho de sucesso (RST em vez de FIN).

---

## 1. Chamadas OT síncronas

O follow-up (OTSetNonBlocking pós-OTOpenEndpoint) foi feito. Em `open_and_connect`:

- `src/cq_transport_ot.c:189-191` — `OTSetSynchronous` + `OTSetNonBlocking` +
  `OTUseSyncIdleEvents(false)` logo após `OTOpenEndpoint`. Sync+non-blocking é
  o idioma correto para poll no WaitNextEvent loop.
- `OTConnect` (`:203-206`) trata `kOTNoDataErr` como "in progress" →
  `ST_CONNECTING`, completado via `OTLook`/`OTRcvConnect` em `pump_connect`
  (`:209-220`). Não bloqueia.
- `OTSnd` (`:235-237`) trata `kOTFlowErr` como "tenta no próximo poll".
- `OTRcv` (`:249-254`) trata `kOTNoDataErr` como "nada agora".

Exceções (bloqueantes de verdade):

| Onde | O quê | Gravidade |
|---|---|---|
| `src/cq_transport_ot.c:167` | `OTInetStringToAddress` — DNS **bloqueante** (o comentário admite) | Latente: host default é dotted-quad (`os9/casquinha.c:68`), então hoje não executa. Mas o Prefs (Fio 6) aceita hostname — um nome digitado congela o loop pelo timeout do resolver, a cada transação (resolve roda em `ST_INIT` de **cada** request). |
| `src/cq_transport_ot.c:183,193` | `OTOpenEndpoint` / `OTBind` síncronos por transação | Rápidos na prática, mas rodam a cada request (ver §2). |
| `os9/casquinha.c:1007` | `ModalDialog` (Prefs); idem `Alert`, `MenuSelect`, `DragWindow`, `TrackControl` | Enquanto UI modal/tracking roda, `PollNetwork` **não é chamado** — transações em voo ficam penduradas do ponto de vista do servidor (socket aberto, ninguém drenando) até o watchdog disparar quando o loop voltar. É o que o servidor vê como "cliente travado". |

## 2. Ciclo de vida de endpoint

**Sim: open→bind→connect→request→close a cada request.** Cada `cq_tx_new` cria
um transporte em `ST_INIT`; o primeiro poll faz `OTOpenEndpoint` + `OTBind` +
resolve + `OTConnect` (`src/cq_transport_ot.c:174-207`); o fim fecha com
`OTUnbind` + `OTCloseProvider`. Para gopher a conexão-por-request é inerente ao
protocolo — o custo evitável é o open/close do *endpoint OT em si*
(reutilizável via unbind → rebind).

**Close do caminho de sucesso está errado:** em `pump_recv`, ao receber
`T_ORDREL` o cliente faz `OTRcvOrderlyDisconnect` mas **nunca envia o seu
próprio** `OTSndOrderlyDisconnect` antes de `OTCloseProvider`
(`src/cq_transport_ot.c:257-264`). O release fica pela metade: o servidor mandou
FIN e espera o FIN do cliente; o close abortivo manda RST. `fail()` e
`cq_tx_cancel()` fazem o `OTSndOrderlyDisconnect` (`:111`, `:337`) — só o
caminho feliz não faz. Idem nos caminhos "got bytes then reset" (`:269`,
`:280`). Para o geomyidae isso vira reset/erro em **toda** transação
bem-sucedida.

## 3. Polling do now-playing (/spot/api/1/now)

- **Intervalo:** 2 s (`CQ_POLL_TICKS 120`, `os9/casquinha.c:70`), disparado em
  `PollNetwork` (`os9/casquinha.c:489-496`), TickCount como único relógio.
  Respeita a lei 5 (≥ micro-cache).
- **Guard de sobreposição:** existe — `if (gTx) { PumpTx(...); return; }`
  (`os9/casquinha.c:487`); o `return` garante que o próximo poll só começa na
  passada seguinte.
- **Background: NÃO pausa.** O event loop (`os9/casquinha.c:1139-1208`) trata só
  `mouseDown`/`keyDown`/`updateEvt` — não há `osEvt`
  (`suspendResumeMessage`). Com o app suspenso, o poll de 2 s, o poll de queue
  de 5 s e fetches de capa continuam batendo no servidor indefinidamente.
- **Poll secundário:** janela Queue aberta re-fetch a cada 5 s
  (`CQ_QUEUE_POLL_TICKS 300`, `os9/casquinha.c:131`, disparo em `:910-918`) —
  também sem pausa em background e **sem backoff** (falha re-marca `gQueueLast`
  e tenta de novo em 5 s fixos, `:906-909`).

## 4. Retry logic

- **Poll /now:** backoff exponencial ok — `cq_backoff` dobra até cap de 30 s em
  falha de transporte ou `error` no corpo (`src/cq_backoff.c:22-28`; alimentado
  em `os9/casquinha.c:346/350/388`). **Sem jitter** (irrelevante com um cliente;
  sincroniza com vários).
- **🔴 Cover fetch: retry storm imediato.** O caminho de **sucesso** marca o
  álbum como tentado (`gCoverAlbum = gCoverReq`, `os9/casquinha.c:463`), mas o
  caminho `CQ_TX_FAILED` só libera o transporte (`os9/casquinha.c:466-468`)
  **sem marcar**. A condição de início (`:471-472`) continua verdadeira → nova
  conexão TCP na passada seguinte do loop. Connection refused falha em
  milissegundos → dezenas de conexões/s. Feedback positivo: servidor sob carga
  → cover falha por timeout → cliente martela mais → servidor pior.
  **Candidato mais forte a assassino do gopher-spot.**
- **Skip encadeado:** double-click na linha N da queue dispara N+1 comandos
  `/next` back-to-back, sem espaçamento (`os9/casquinha.c:766-768`,
  `:851-855`). Rajada proporcional à posição clicada; cada `/next` vira chamada
  Spotify no dcgi.
- **Search:** `RunSearch` cancela-e-substitui a busca em voo
  (`os9/casquinha.c:673`) — cancel não des-envia (lei 1), então Enter repetido
  = múltiplas buscas executando no servidor/Spotify. Sem debounce no campo.

## 5. Requests em voo

Guards individuais existem em todos os canais: `gTx` (poll, `:487`), `gCmd`
(`StartCommand` dropa se ocupado, `:400`), `gFire` (`:632`), `gCover` (`!gCover`
na condição, `:471`), `gQueueTx` (`:655`, `:860`, `:910`), `gSkipsPending`
(`:766`). Prev/next passam pelo `cq_debounce` de ~0,3 s (`:482-485`,
`:529-533`).

**Porém os canais são independentes entre si** — pior caso, um único cliente
mantém **7 conexões TCP simultâneas**: `gTx` + `gCmd` + `gCover` + `gFire` +
`gSearchTx` + `gQueueTx` + `gPls`, mais o stream Icecast do QuickTime por fora.
Num geomyidae fork-per-connection, cada Casquinha vale até ~8 processos.

## 6. Timeouts

Existem e são razoáveis: connect deadline 2 s e watchdog total 5 s, checados a
cada `cq_tx_poll` (`src/cq_transport_ot.c:312-319`; constantes em
`src/cq_transport.h:27-28`). `OTRcv` nunca bloqueia (§1), então o WNE loop não
trava por rede. Ressalvas:

- O deadline de 2 s cobre só `ST_CONNECTING`; um `ST_RECEIVING` estagnado só cai
  no watchdog de 5 s. Aceitável.
- O watchdog **só avança quando o loop roda** — durante
  `ModalDialog`/`TrackControl`/drag (§1), a transação congela e o servidor
  segura o socket até o loop voltar. Não é timeout do transporte, é starvation
  do poll.

---

## CLIENTS.md compliance (guia do backend, `gopher-spot/CLIENTS.md`)

Avaliação das 21 guidelines + checklist de 12 itens contra o código
(2026-07-05). Veredito: ~14 já cumpridas por construção; 2 violações reais;
4 divergências menores; nada inviável no OS 9/Retro68 (o TEC já está linkado
para o sentido inverso da conversão).

| Guideline / checklist | Status | Referência |
|---|---|---|
| g1 cadência ≥2 s fixa | ✅ | `CQ_POLL_TICKS 120`, backoff só alarga (`os9/casquinha.c:70`) |
| g2 interpolar com ts | ✅ | barra interpola entre polls (`os9/casquinha.c:246-249`) |
| g3 newest-wins por ts | ✅ | `cq_guard_accept_ts` (`src/cq_guard.c:8`) |
| g4 sem /now pós-comando | ✅/⚠️ | resposta do comando adotada (`PumpTx`); mas `gLastPoll` não re-marca → checklist 4 pode falhar por coincidência (**Fio G**) |
| g5 switch no código, não em message | ✅ | `AdoptReply` compara só `error`; `message` é display |
| g6 rate_limited: manter cadência+snapshot | ⚠️ | backoff em qualquer erro; compara `"upstream"`, nunca casa `rate_limited` (**Fio G**) |
| g7 sem tight-loop de comando | ✅/⚠️ | comandos não retentam; /next encadeado do jump tensiona o espírito (**Fio D**) |
| g8 sem retry-loop de not_found | ⚠️ | o cover storm é exatamente um retry-loop em falha de transporte (**Fio A**) |
| g9 cache de covers client-side | ⚠️ | um único GWorld; A→B→A refetch (**Fio A** estendido) |
| g10 fetch só em album_id novo | ✅ | `os9/casquinha.c:471-472` |
| g11 só tamanhos canônicos | ✅ | só 64 (`CQ_COVER_PX`) |
| g12 sniff JPEG / erro em texto | ✅/⚠️ | `cq_data_is_jpeg` sim; código de erro do corpo não é lido (menor) |
| g13 queue/add: UM re-poll após pausa | ⚠️ | refetch imediato (perde a janela 1-2 s) + poll fixo 5 s (**Fio H**) |
| g14 não gatear em queue_len | ✅ | `queue_len` não é usado em lógica |
| g15 busca no submit | ✅ | Return/botão, nunca por tecla |
| g16 query em UTF-8 percent-encoded | 🔴 | `RunSearch` encoda bytes MacRoman crus — acentos corrompem (**Fio F**) |
| g17 playlists/forbidden | n/a | sem feature de playlist |
| g18 wake só por intenção humana | ✅ | `wake` nem existe no cliente |
| g19 confiar nos clamps do servidor | ✅ | clamps locais só como UX |
| g20 consistência eventual seek/volume | ✅ | `gVolHold` 3 s, sem re-emissão |
| g21 parse forward-compatível | ✅ | `cq_codec` ignora chaves desconhecidas, last-wins, qualquer ordem — checklist 11 passa por construção |

Checklist: 1✅ 2✅ 3✅ 4⚠️(G) 5⚠️(G) 6🔴(A) 7✅ 8🔴(F) 9⚠️(H) 10✅ 11✅ 12✅.

---

## Fase 2 — fios propostos (AGUARDANDO OK; ordem = prioridade)

Regras: contrato aditivo (nada de API breaking), string de teste UTF-8 canônica
`Construção`, Retro68 PPC (--universal), sem dependências novas, cada fio com
teste host ou plano de verificação manual no OS 9/UTM.

### Fio A — cover fail-once + tried-set (mata o storm) 🔴 — IMPLEMENTADO (b8/b9), aguardando verificação UTM
Implementação (2026-07-05): módulo puro novo `src/cq_cache.c/.h` — cache FIFO
de slots fixos (N=8, chave string ≤63 bytes) guardando payload opaco, onde
entrada presente com valor NULL = "tentado, sem imagem" (o fail-once) e valor
não-NULL = GWorld decodificado (o antigo opcional-LRU, unificado). O app
(`os9/casquinha.c`) trocou `gCoverGW`/`gCoverAlbum` por `gCovers`: TODO
desfecho do fetch (decodificado / não-JPEG / falha de transporte) é gravado no
cache, o início de fetch exige `!cq_cache_has`, o draw busca pelo `album_id`
corrente (nunca mostra capa do vizinho), e o Prefs drena o cache ao trocar de
host. Memória: 64×64×32bpp = 16 KB por slot decodificado, ≤128 KB. Teste host:
`tests/cache_test.c` (FIFO, negativo, evicção, chave `Construção`) — suite
152/152 verde; cross-build Retro68 limpo, `b8` no share.
Verificação UTM pendente: derrubar o `/cover` no servidor, confirmar UMA
tentativa por álbum (logs do geomyidae), `/now` seguindo normal com
`Construção` no título renderizando; alternar dois álbuns e confirmar zero
re-requests e capa correta em cada um.

### Fio B — orderly release completo — IMPLEMENTADO (b9), aguardando verificação UTM
`OTSndOrderlyDisconnect` no caminho `T_ORDREL` de `pump_recv` antes do close
(`src/cq_transport_ot.c:257-264`), simetria com `fail`/`cancel`. Verificação:
tcpdump no lado do servidor — FIN/FIN-ACK limpo em transação de sucesso, sem
RST; logs do geomyidae sem reset por request.

### Fio C — suspend/resume — IMPLEMENTADO (b9), aguardando verificação UTM
Tratar `osEvt`/`suspendResumeMessage` no event loop; enquanto suspenso, pausar
os disparos de poll (`gLastPoll`, `gQueueLast`, início de cover) — transações
já em voo drenam. No resume, poll imediato. Verificação UTM: app em background
→ contador `gPolls` para de subir; volta ao foreground → retoma em ≤2 s.

### Fio D — jitter + backoff nos secundários — IMPLEMENTADO (b9), aguardando verificação UTM
Jitter no `cq_backoff` (aditivo: nova função, ex. `cq_backoff_interval_j`, sem
quebrar a atual); backoff no re-fetch de queue em falha (hoje 5 s fixos,
`os9/casquinha.c:906-918`); espaçamento mínimo (~0,5 s) entre `/next`
encadeados do jump de queue. Teste host: `tests/backoff_test.c` estendido
(jitter dentro de faixa, determinístico com seed injetada — sem clock/rand
dentro do módulo puro).

### Fio E — teto de conexões simultâneas (menor) — IMPLEMENTADO (b9; sem reuso de endpoint), aguardando verificação UTM
Contador global de transações em voo; cover/fire/queue esperam quando ≥N (ex.
N=3); opcional: reuso do endpoint OT entre transações (unbind→rebind) para
poupar open/close. Verificação: diagnóstico na status row mostrando in-flight
máximo observado.

### Fio F — busca em UTF-8 na fronteira de submit (CLIENTS.md g16) 🔴 — IMPLEMENTADO (b9), aguardando verificação UTM
O campo de busca é um edit-text clássico: `GetControlData` devolve bytes
MacRoman, e `RunSearch` (`os9/casquinha.c:661-676`) percent-encoda esses bytes
crus via `EscInto` (`:565`). `Construção` digitado NÃO chega como
`constru%C3%A7%C3%A3o` — acentos corrompem no servidor (checklist 8 falha).
Fix: segundo `TECCreateConverter` MacRoman→UTF-8 ao lado do `gConv`
(`os9/casquinha.c:1110-1115`) e conversão em `RunSearch` ANTES do `EscInto` —
simétrico à fronteira de desenho existente (`ToMacRoman`, `:146`).
Verificação UTM: buscar `Construção` → o log do geomyidae mostra
`constru%C3%A7%C3%A3o` (`kubectl -n gopher-spot logs deploy/gopher-server |
grep serving`); resultados com acento renderizam de volta corretos.

### Fio G — vocabulário de erros + pós-comando (CLIENTS.md g6, checklist 4) — IMPLEMENTADO (b9), aguardando verificação UTM
`AdoptReply` (`os9/casquinha.c:339-346`) faz backoff em QUALQUER doc de erro e
o special-case compara `"upstream"` — o código real do throttling é
`rate_limited` (API.md, fio 429), então a mensagem amigável nunca dispara.
Fix: `rate_limited` → manter cadência (NÃO chamar `cq_backoff_fail`), manter
snapshot, mensagem calma; `upstream`/demais → backoff como hoje (continuar
comparando só o código, nunca `message`). E re-marcar `gLastPoll` quando a
resposta de um comando é adotada (`PumpTx` com `isPoll==0`), garantindo ≥1
intervalo até o próximo poll (checklist 4: nenhum `/now` ≤1 s após comando).
Verificação UTM: simular 429 no servidor → cadência de poll inalterada,
último snapshot mantido; comando → nenhum `/now` no primeiro segundo seguinte
(logs do geomyidae).

### Fio H — queue/add re-poll educado (CLIENTS.md g13; menor) — IMPLEMENTADO (b9), aguardando verificação UTM
O refetch de `/queue` pós-fire é imediato (`os9/casquinha.c:858-863`) e tende
a PERDER a janela de consistência eventual de 1-2 s. Fix: agendar UM re-poll
~2 s (120 ticks) após o fire completar, em vez do imediato. Manter o poll de
5 s da janela aberta (defensável: a fila muda por consumo embaixo de nós),
anotando a tensão com a guideline 14 (fetch-on-open).
Verificação UTM: queue/add → exatamente um `/queue` extra ~2 s depois, com o
item já presente (checklist 9).

### Não-fio: notifier proc
Decidido NÃO migrar para `OTInstallNotifier`: o modelo atual (sync+non-blocking
+ poll no idle loop) é o recomendado para app cooperativo single-thread no
OS 9 — notifier roda em deferred-task time e traz reentrância sem ganho aqui.
DNS bloqueante (§1) fica documentado como limitação; se hostname virar caso
real, tratar em fio próprio (resolver assíncrono via `OTInetStringToAddress`
em modo async é outro animal).

---

## Histórico de interações

### Audit Casquinha vs CLIENTS.md (2026-07-05)

O guia do backend (`gopher-spot/CLIENTS.md`, 21 guidelines + checklist de 12
itens) foi avaliado contra o código — resultado na seção "CLIENTS.md
compliance" acima: ~14 guidelines já cumpridas por construção, 2 violações
reais (encoding MacRoman na busca, g16/checklist 8; cover storm + refetch em
alternância, checklist 6) e 4 divergências menores. A avaliação gerou os fios
F/G/H e estendeu o Fio A. Na sequência, os fios A–H foram TODOS implementados
(builds b8/b9, no share): suite host 156/156 verde, cross-build Retro68 sem
warnings. Pendência única: o passe de verificação UTM no OS 9, fio a fio,
conforme os passos de verificação de cada seção — e o spot-check final do
CLIENTS.md (itens 1-12) observando uma sessão inteira nos logs do geomyidae:

```sh
kubectl -n gopher-spot logs deploy/gopher-server --since=10m \
  | grep 'serving' | awk '{print $1, $NF}'
```
