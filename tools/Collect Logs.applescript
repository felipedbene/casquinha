-- Collect Logs - copy every per-build Casquinha log onto the AFP share.
-- Open in Script Editor ON THE OS 9 VM, adjust the two properties, Run.
-- Logs live next to the app as "Casquinha <tag>.log" (one file per build).

property shareName : "share" -- the mounted netatalk volume's name (see Chooser)
property appFolderPath : "Macintosh HD:Casquinha:" -- folder holding the app + logs

tell application "Finder"
	set logFiles to every file of folder appFolderPath whose name begins with "Casquinha b"
	if (count of logFiles) is 0 then
		display dialog "No Casquinha logs found in " & appFolderPath buttons {"OK"} default button 1
	else
		repeat with f in logFiles
			duplicate f to disk shareName with replacing
		end repeat
		display dialog ((count of logFiles) as string) & " log(s) copied to " & shareName & "." buttons {"OK"} default button 1
	end if
end tell
