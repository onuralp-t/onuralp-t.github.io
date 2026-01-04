* [go back to README](README.md)
# Set up a free tier Google Cloud VM
## Quotas and Limits
* https://docs.cloud.google.com/free/docs/free-cloud-features#compute
* https://cloud.google.com/compute/all-pricing
* https://cloud.google.com/vpc/network-pricing?hl=en
## Before creating VM: allowing IPV6 in VPC network
* Google Cloud removed external ipv4 addresses from free tier, but external ipv6 addresses are still allowed for free. (https://cloud.google.com/vpc/network-pricing?hl=en)
* To use external ipv6 addresses, we have to switch to **Premium** Tier Network instead of Standard, which means our general 200 GB egress allowance is down to **1 GB egress** allowence only to specific countries, and we get charged money if some random restricted country pings us.
### Create a new IPv6 enabled VPC network
* create VPC network named "us-east1-ipv6" in auto mode, MTU 1460, routing regional, path selection legacy, uncheck all firewall rules, default settings otherwise
* edit it and change Subnet creation mode to Custom
* edit the auto-generated us-east1 subnet to be dual-stack ipv4 + ipv6 and set ipv6 to external
### Setup Firewall rules of your new VPC network
* allow ::/0 to tcp:22 (ipv6 ssh)
* allow 35.235.240.0/20 to tcp:22 (Google Cloud website SSH button)
* allow ::/0 to tcp:80,443 and udp:443
    * (for ipv6 web traffic, could result in massive egress to restricted countries in form of refused HTTP requests, **COSTS MONEY**) (Need cloudflare to block requests on a per-country basis) (cloudflare requires domain) (use free domain?)
* allow 0.0.0.0/0 to icmp (icmp is important? prolly results in tiny egress to restricted countries)
* allow 10.0.0.0/8 to all protocols (internal google communication, should be safe?)
## Create VM
* WHAT TO SELECT:
    * us-east1
    * Ubuntu 24.04 LTS Minimal
    * No snapshots, no backups
    * 30 GB Standard Persistent Disk
    * Choose previously created VPC network and subnetwork
    * Choose IPv4 and IPv6 (dual-stack)
    * Premium Tier Network (automatically chosen after dual-stack selection)
        * Egress is **Free** to: North America, Europe, (some Asia)
        * Egress is **NOT Free** to **regions**: South America, Middle East, Africa
        * Egress is **NOT Free** to **countries**: Australia, Indonesia, Korea, China, Saudi Arabia
        * source: https://cloud.google.com/vpc/network-pricing
    * Choose External IPv4 address as none, External IPv6 address as Ephemeral (Automatic)
    * Don't install Ops Agent
    * Default settings for security tab and advanced tab
```
gcloud compute instances create ecommerce-vm \
    --project=ecommerce-project-457223 \
    --zone=us-east1-c \
    --machine-type=e2-micro \
    --network-interface=network-tier=STANDARD,stack-type=IPV4_ONLY,subnet=default \
    --maintenance-policy=MIGRATE \
    --provisioning-model=STANDARD \
    --service-account=814607272924-compute@developer.gserviceaccount.com \
    --scopes=https://www.googleapis.com/auth/devstorage.read_only,https://www.googleapis.com/auth/logging.write,https://www.googleapis.com/auth/monitoring.write,https://www.googleapis.com/auth/service.management.readonly,https://www.googleapis.com/auth/servicecontrol,https://www.googleapis.com/auth/trace.append \
    --tags=http-server,https-server \
    --create-disk=auto-delete=yes,boot=yes,device-name=ecommerce-vm,image=projects/debian-cloud/global/images/debian-12-bookworm-v20250415,mode=rw,size=30,type=pd-standard \
    --no-shielded-secure-boot \
    --shielded-vtpm \
    --labels=goog-ec-src=vm_add-gcloud \
    --reservation-affinity=any
```
