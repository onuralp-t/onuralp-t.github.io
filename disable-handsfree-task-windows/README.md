# windows task
* win + r `eventvwr.msc`, Applications and Services Logs -> Microsoft -> Windows -> Bluetooth-Policy -> Operational, right click Operational and press `Enable Log` (This makes it so that whenever a bluetooth device connects, an Event with ID 9 gets emmitted)
* this exported task assumes the path of the script is like `"C:\opt\scripts\disable_handsfree.ps1"`, might need to change the path in xml before importing, or maybe edit the task afterwards idk
