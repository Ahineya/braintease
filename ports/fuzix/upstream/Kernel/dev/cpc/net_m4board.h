#ifdef CONFIG_NET_M4BOARD

struct m4_network_config {
    char name[16];
    uint8_t ssid[32];
    uint8_t password[64];
    uint32_t ip;
    uint32_t nm;
    uint32_t gw;
    uint32_t dns1;
    uint32_t dns2;
    uint32_t dhcp;
    int32_t timezone;
    uint8_t ntpserver[48];
    uint8_t reserved1;
    uint8_t reserved2;
    uint8_t mac[6];
};

struct m4_sockinfo {
    uint8_t status;
    uint8_t lastcmd;
    uint16_t received;
    uint32_t ip_addr;
    uint16_t port;
};

extern uint8_t m4_present;
extern uint8_t m4_net_connect_socket;
extern uint32_t m4_net_connect_ip;
extern uint16_t m4_net_connect_port;
extern uint8_t m4_net_close_socket;
extern uint8_t m4_net_bind_socket;
extern uint32_t m4_net_bind_ip;
extern uint16_t m4_net_bind_port;
extern uint8_t m4_net_listen_socket;
extern uint8_t m4_net_accept_socket;
extern uint8_t m4_net_send_socket;
extern uint16_t m4_net_send_len;
extern uint8_t m4_net_recv_socket;
extern uint16_t m4_net_recv_len;
extern uint8_t m4_net_raw;

extern uint8_t m4_net_socket(void);
extern uint8_t m4_net_connect(void);
extern uint8_t m4_net_close(void);
extern uint8_t m4_net_bind(void);
extern uint8_t m4_net_listen(void);
extern uint8_t m4_net_accept(void);
extern uint8_t m4_net_hostip_start(char *hostname); /*Always check strlen(hostname)>0 before call*/
extern uint8_t m4_net_send(void *buf); /*Always check (len>0 && len<=2048) before call*/
extern uint16_t m4_net_recv(void *buf);
extern uint8_t m4_net_sock_info(struct m4_sockinfo *sockinfo);
extern uint8_t m4_net_getnetwork(void); /*fills m4_network_config struct*/
extern uint8_t m4_net_setnetwork(char *cfg);
/*
cfg pointer to parameter config string for m4:

name 	netbios name, only use UPPERCASE letters and numbers.
ssid 	your wireless accesspoint/router name (remember name is case sensitive).
pw 		password for your wireless ap/router.
dhcp		0=disable DHCP and use static ip settings, 1=use DHCP (static IP settings are ignored)
ip		static ip number for your CPC
gw		gateway for your network
nm		netmask for your network (usually 255.255.255.0)
dns1		dns server, you can use ie. 8.8.8.8 (google dns)
dns2		dns server backup, you can use ie. 8.8.4.4 (google dns)
ntp		ntp time server, this will be used to retrieve time
tz		time zone, can be +/- 12, set it to your time zone
start	when start is set to 1, settings are not applied yet.
		This is if your parameters are longer than 255 chars (cpc basic line limit), then you can issue the command twice.
Example usage, using DHCP:
"name=CPC6128, ssid=NETGEAR, pw=12345678, dhcp=1, dns1=8.8.8.8, dns2=8.8.4.4"
*/

#endif
