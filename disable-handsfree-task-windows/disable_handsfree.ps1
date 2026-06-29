# Get all active Bluetooth Hands-Free PnP devices
$devices = Get-PnpDevice | Where-Object { 
    ($_.InstanceId -like "*BTHENUM*" -or $_.InstanceId -like "*BthHFEnum*") -and 
    ($_.FriendlyName -like "*Hands-Free*" -or $_.FriendlyName -like "*Handsfree*") -and 
    $_.Status -eq "OK" 
}

# Disable each discovered Hands-Free component
foreach ($device in $devices) {
    try {
        Disable-PnpDevice -InstanceId $device.InstanceId -Confirm:$false -ErrorAction Stop
        Write-Output "Successfully disabled PnP node: $($device.FriendlyName)"
    } catch {
        Write-Error "Failed to disable $($device.FriendlyName): $_"
    }
}