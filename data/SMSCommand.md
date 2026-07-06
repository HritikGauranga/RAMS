
Command---------------------seconds-----Result
Set Relay%1234,Siren,ON,0	0	        Permanently ON
Set Relay%1234,Siren,ON,	blank       Permanently ON
Set Relay%1234,Siren,ON,30	30	        ON for 30s then auto OFF
Set Relay%1234,Siren,OFF,0	any	        Permanently OFF, cancels any active pulse


Command	-------------------------------------Response
GET STATUS	                                 System info (site, serial, uptime, alarms, signal, relays)
GET INPUT	                                 DI1-DI4 states + AI1-AI2 values with alarm status
GET RELAY	                                 DO1-DO2 names, ON/OFF state
GET IP%<PIN>                             	 IP address, DHCP mode, gateway
Set Relay%<PIN>,<Name>,<ON|OFF>,<seconds>	 Controls relay output
