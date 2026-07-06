# Backend Exhaustion Audit — Casquinha vs gopher-spot

Data do audit: 2026-07-05 · Escopo: cliente Casquinha (OS 9/OT) exaurindo o
gopher-spot (geomyidae + dcgi, K8s, porta 70).

**Estado: Fase 1 (diagnóstico) e Fase 2 (fios A–H) concluídas em código** —
builds b8/b9 no share, suite host 156/156 verde, cross-build Retro68 limpo.
**Pendência única: o passe de verificação UTM no OS 9**, fio a fio (passos em
cada seção abaixo), fechando com o spot-check do CLIENTS.md observando uma
sessão inteira nos logs do geomyidae:

```sh
kubectl -n gopher-spot logs deploy/gopher-server --since=10m \
  | grep 'serving' | awk '{print $1, $NF}'
```

## O que o diagnóstico encontrou (resumo)

A disciplina sync+non-blocking do Open Transport já estava correta; os
culpados reais eram outros, todos corrigidos pelos fios:

- **Retry storm no fetch de capa** — falha de transporte não marcava o álbum
  como "tentado"; nova conexão TCP a cada passada do loop (→ **Fio A**).
- **Close abortivo no caminho de sucesso** — `T_ORDREL` recebido sem responder
  o próprio orderly release: RST em vez de FIN em toda transação boa (→ **B**).
- **Polling seguia em background** — nenhum tratamento de suspend/resume (→ **C**).
- **Sem jitter/backoff nos canais secundários** e `/next` encadeado sem
  espaçamento (→ **D**).
- **Até 7 conexões TCP simultâneas** de um único cliente (→ **E**, teto de 3
  para starters automáticos).
- **Busca percent-encodava bytes MacRoman crus** — acentos corrompiam (→ **F**).
- **`rate_limited` nunca casava** (comparava `"upstream"`) e comando não
  re-marcava o relógio do poll (→ **G**).
- **Refetch de queue pós-add imediato** perdia a janela de consistência
  eventual (→ **H**).

Limitação documentada (sem fio): DNS bloqueante em `OTInetStringToAddress` —
só executa se o Prefs receber hostname em vez de dotted-quad; tratar em fio
próprio se virar caso real.

## CLIENTS.md compliance (guia do backend, `gopher-spot/CLIENTS.md`)

21 guidelines + checklist de 12 itens. Estado pós-fios (em código; UTM pendente):

| Guideline / checklist | Status | Referência |
|---|---|---|
| g1 cadência ≥2 s fixa | ✅ | `CQ_POLL_TICKS 120`, backoff só alarga |
| g2 interpolar com ts | ✅ | barra interpola entre polls |
| g3 newest-wins por ts | ✅ | `cq_guard_accept_ts` |
| g4 sem /now pós-comando | ✅ | resposta adotada re-marca `gLastPoll` (Fio G) |
| g5 switch no código, não em message | ✅ | `AdoptReply` compara só `error` |
| g6 rate_limited: manter cadência+snapshot | ✅ | sem backoff, snapshot mantido (Fio G) |
| g7 sem tight-loop de comando | ✅ | `/next` encadeado pace ~0,5 s (Fio D) |
| g8 sem retry-loop de not_found | ✅ | fail-once via `cq_cache` (Fio A) |
| g9 cache de covers client-side | ✅ | FIFO 8 slots, A→B→A sem refetch (Fio A) |
| g10 fetch só em album_id novo | ✅ | gate por `cq_cache_has` |
| g11 só tamanhos canônicos | ✅ | só 64 (`CQ_COVER_PX`) |
| g12 sniff JPEG / erro em texto | ✅/⚠️ | `cq_data_is_jpeg` sim; código de erro do corpo não é lido (menor, aceito) |
| g13 queue/add: UM re-poll após pausa | ✅ | kick único ~2 s pós-fire (Fio H) |
| g14 não gatear em queue_len | ✅ | `queue_len` não é usado em lógica |
| g15 busca no submit | ✅ | Return/botão, nunca por tecla |
| g16 query em UTF-8 percent-encoded | ✅ | TEC MacRoman→UTF-8 antes do escape (Fio F) |
| g17 playlists/forbidden | n/a | sem feature de playlist |
| g18 wake só por intenção humana | ✅ | `wake` nem existe no cliente |
| g19 confiar nos clamps do servidor | ✅ | clamps locais só como UX |
| g20 consistência eventual seek/volume | ✅ | `gVolHold` 3 s, sem re-emissão |
| g21 parse forward-compatível | ✅ | `cq_codec` ignora chaves desconhecidas |

Checklist 1–12: tudo ✅ em código (g12 com divergência menor aceita); a prova
final é o spot-check UTM acima.

---

## Fios A–H — implementados (b8/b9), verificação UTM pendente

Regras que valeram para todos: contrato aditivo (nada de API breaking), string
de teste UTF-8 canônica `Construção`, Retro68 PPC (--universal), sem
dependências novas, cada fio com teste host ou plano de verificação manual.

### Fio A — cover fail-once + tried-set (matou o storm)
Módulo puro `src/cq_cache.c/.h` — cache FIFO de slots fixos (N=8, chave string
≤63 bytes): entrada com valor NULL = "tentado, sem imagem" (fail-once), valor
não-NULL = GWorld decodificado. TODO desfecho do fetch é gravado; início de
fetch exige `!cq_cache_has`; o draw busca pelo `album_id` corrente; Prefs
drena o cache ao trocar de host. ≤128 KB. Teste host: `tests/cache_test.c`.
**Verificação UTM:** derrubar o `/cover` no servidor → UMA tentativa por álbum
(logs do geomyidae), `/now` seguindo normal com `Construção` renderizando;
alternar dois álbuns → zero re-requests, capa correta em cada um.

### Fio B — orderly release completo
`OTSndOrderlyDisconnect` no caminho `T_ORDREL` de `pump_recv` antes do close,
simetria com `fail`/`cancel`. **Verificação:** tcpdump no lado do servidor —
FIN/FIN-ACK limpo em transação de sucesso, sem RST; logs do geomyidae sem
reset por request.

### Fio C — suspend/resume
`osEvt`/`suspendResumeMessage` no event loop (+ `SIZE` com canBackground);
suspenso: nenhum poll NOVO dispara, transações em voo drenam, áudio segue. No
resume, poll imediato. **Verificação UTM:** app em background → contador de
polls para de subir; foreground → retoma em ≤2 s.

### Fio D — jitter + backoff nos secundários
`cq_backoff_interval_seeded` (jitter positivo determinístico, seed injetada —
sem clock/rand no módulo puro; `tests/backoff_test.c`); re-fetch de queue
backs off em falha (`gQBackoff`); `/next` encadeados espaçados ~0,5 s
(`CQ_SKIP_GAP_TICKS`). **Verificação UTM:** queue jump na linha N → hops com
~0,5 s entre si nos logs.

### Fio E — teto de conexões simultâneas
Starters automáticos (cover, queue poll) esperam quando ≥3 em voo
(`CQ_MAX_INFLIGHT`, `TxInFlight()`). Reuso de endpoint OT (unbind→rebind) foi
avaliado e NÃO feito — custo de open/close por request é aceitável para
gopher. **Verificação:** in-flight máximo observado ≤3 para starters
automáticos durante uso pesado.

### Fio F — busca em UTF-8 na fronteira de submit (g16)
Segundo `TECCreateConverter` MacRoman→UTF-8 (`gConvOut`) e conversão em
`RunSearch` ANTES do percent-encode — simétrico à fronteira de desenho
(`ToMacRoman`). **Verificação UTM:** buscar `Construção` → log do geomyidae
mostra `constru%C3%A7%C3%A3o`; resultados com acento renderizam de volta
corretos.

### Fio G — vocabulário de erros + pós-comando (g6, checklist 4)
`rate_limited` → mantém cadência (sem `cq_backoff_fail`), mantém snapshot,
mensagem calma; demais códigos → backoff como antes (comparando só o código,
nunca `message`). Resposta de comando adotada re-marca `gLastPoll` → ≥1
intervalo até o próximo `/now`. **Verificação UTM:** simular 429 → cadência
inalterada, snapshot mantido; comando → nenhum `/now` no primeiro segundo
seguinte.

### Fio H — queue/add re-poll educado (g13)
Refetch pós-fire virou UM kick agendado ~2 s (120 ticks) depois
(`gQueueKick`), respeitando a janela de consistência eventual; poll de 5 s da
janela aberta mantido (a fila muda por consumo embaixo de nós). **Verificação
UTM:** queue/add → exatamente um `/queue` extra ~2 s depois, com o item já
presente.

### Não-fio: notifier proc
Decidido NÃO migrar para `OTInstallNotifier`: o modelo atual
(sync+non-blocking + poll no idle loop) é o recomendado para app cooperativo
single-thread no OS 9 — notifier roda em deferred-task time e traz reentrância
sem ganho aqui.
