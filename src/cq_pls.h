/*
 * cq_pls — Casquinha helper (pure): first stream URL from a PLS/M3U playlist.
 *
 * Ports DeGelato's DGPLSParser. Used by the (deferred) audio path to find the
 * Icecast stream URL from /spot/stream.pls. Ported now because it is pure and
 * cheap. Rules: a PLS `File<n>=URL` with the LOWEST index wins and beats any
 * bare `http(s)://` line; `#`-comments (M3U directives) are skipped; a bare URL
 * is the fallback (first one wins).
 */
#ifndef CQ_PLS_H
#define CQ_PLS_H

/*
 * Returns a newly-allocated URL string (caller frees), or NULL if none found.
 * A NULL or empty text yields NULL.
 */
char *cq_pls_first_url(const char *text);

#endif /* CQ_PLS_H */
