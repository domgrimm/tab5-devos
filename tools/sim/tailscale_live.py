#!/usr/bin/env python3
import json
import subprocess
import sys

def main():
    try:
        res = subprocess.run(["tailscale", "status", "--json"], capture_output=True, text=True, timeout=3)
        if res.returncode != 0:
            sys.exit(1)
        data = json.loads(res.stdout)
    except Exception as e:
        sys.exit(1)

    self_node = data.get("Self", {})
    dns = data.get("MagicDNSSuffix", "ts.net")
    s_name = self_node.get("HostName", "dev-server")
    s_ips = self_node.get("TailscaleIPs", ["127.0.0.1"])
    s_ip = s_ips[0] if s_ips else "127.0.0.1"
    s_relay = self_node.get("Relay", "syd")
    s_rx = self_node.get("RxBytes", 0)
    s_tx = self_node.get("TxBytes", 0)

    print(f"SELF|{s_name}|{dns}|{s_ip}|{s_relay}|{s_rx}|{s_tx}")

    peers = list(data.get("Peer", {}).values())
    # Sort online first, then active/recent
    peers.sort(key=lambda p: (not p.get("Online", False), not p.get("Active", False), p.get("HostName", "").lower()))

    for p in peers[:16]:
        name = p.get("HostName", "peer")
        fqdn = p.get("DNSName", name).rstrip(".")
        p_ips = p.get("TailscaleIPs", [""])
        pip = p_ips[0] if p_ips else ""
        os_name = p.get("OS", "unknown")
        online = 1 if p.get("Online", False) else 0
        direct = 1 if p.get("CurAddr") else 0
        prx = p.get("RxBytes", 0)
        ptx = p.get("TxBytes", 0)
        relay_p = p.get("Relay", "")
        # Compute last seen in seconds if offline
        last_seen_sec = 0
        print(f"PEER|{name}|{fqdn}|{pip}|{os_name}|{direct}|{online}|{prx}|{ptx}|{relay_p}|{last_seen_sec}")

if __name__ == "__main__":
    main()
