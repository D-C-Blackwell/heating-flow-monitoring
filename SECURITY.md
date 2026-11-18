# Security and privacy

Never attach credentials, a network configuration, real device logs or a full
flash dump to a public issue. Use synthetic entities/readings when reporting a
problem. Credentials are entered through the device portal and stored locally.
The display uses unencrypted MQTT on a trusted local network; do not expose
its setup portal or broker directly to the Internet. The project is a monitor,
not a heating safety or control system. Avoid opening an issue containing a
secret; rotate any credentials accidentally disclosed.
