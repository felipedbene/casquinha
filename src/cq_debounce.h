/*
 * cq_debounce — Casquinha Reconciler (pure): the pre-wire coalescer.
 *
 * Ports DeGelato's DGDebouncer. CLIENT-PATTERN.md §2 law 1 / DeGelato R1:
 * CANCEL != UN-SEND. On the LAN the selector is on the wire within one loop
 * turn, so gopher-spot executes every command that reaches transport; a
 * client-side cancel only stops us listening. So rapid transport taps must be
 * coalesced BEFORE the wire — three fast Next taps skip one track, not three.
 *
 * This is a one-slot, last-value-wins holder. The *when to flush* timer lives in
 * the controller (a TickCount deadline on OS 9); this object holds only the
 * coalesced value and guarantees no double-send.
 */
#ifndef CQ_DEBOUNCE_H
#define CQ_DEBOUNCE_H

typedef struct {
    char *pending;   /* owned copy, or NULL when empty */
} cq_debounce;

void cq_debounce_init(cq_debounce *d);

/* Replace any pending value (last wins). A NULL value clears the slot. */
void cq_debounce_set(cq_debounce *d, const char *value);

int cq_debounce_has(const cq_debounce *d);

/*
 * Hand the caller the pending value and clear the slot. Ownership transfers:
 * the caller must free() the returned string. Returns NULL when empty — a second
 * take() with no intervening set() returns NULL (the no-double-send guarantee).
 */
char *cq_debounce_take(cq_debounce *d);

void cq_debounce_free(cq_debounce *d);

#endif /* CQ_DEBOUNCE_H */
