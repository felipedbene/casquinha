-- Test Casquinha - ordered smoke test (b36+)
-- Open this in Script Editor ON THE OS 9 VM and press Run.
-- Drives the app through Apple Events (misc/dosc = "do script"); every step
-- is answered by an "apple-event: ..." line in the per-build Casquinha log,
-- so the run narrates itself. Requires the b36+ binary (high-level events).
--
-- Sequence: launch -> listen -> search -> enqueue -> next -> wake -> stop -> quit

property testQuery : "queen"

tell application "Casquinha"
	activate
end tell
delay 5 -- boot, first /now poll, cover fetch

tell application "Casquinha"
	«event miscdosc» "listen"
end tell
delay 25 -- prebuffer (~2 s) + an audible playback window

tell application "Casquinha"
	«event miscdosc» ("search:" & testQuery)
end tell
delay 6 -- results arrive

tell application "Casquinha"
	«event miscdosc» "add" -- enqueue the first result
end tell
delay 6 -- queue kick refetch (~2 s) + a poll

tell application "Casquinha"
	«event miscdosc» "next"
end tell
delay 15 -- hear the track change ride through the radio latency

tell application "Casquinha"
	«event miscdosc» "wake"
end tell
delay 10

tell application "Casquinha"
	«event miscdosc» "stop"
end tell
delay 3

tell application "Casquinha"
	quit
end tell

display dialog "Casquinha smoke test finished - now run Collect Logs." buttons {"OK"} default button 1
