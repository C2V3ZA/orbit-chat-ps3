"""Simulated home router for tests: SSDP + UPnP IGD (SOAP) and a PCP server on 127.0.0.1.
Mode 'upnp': answers SSDP/IGD, maps ports. Mode 'pcp': no UPnP, PCP MAP only.
Mode 'cgnat': UPnP answers but the external address is 100.64.x (carrier-grade NAT)."""
import http.server, socket, struct, sys, threading, re

mode = sys.argv[1]
log = open(sys.argv[2], 'w')
HTTP_PORT = 49152
mappings = {}

DESC = """<?xml version="1.0"?><root xmlns="urn:schemas-upnp-org:device-1-0"><device>
<deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType><deviceList><device>
<deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType><deviceList><device>
<deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType><serviceList><service>
<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
<serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>
<controlURL>/upnp/control/WANIPConn1</controlURL></service></serviceList>
</device></deviceList></device></deviceList></device></root>"""

def envelope(action, inner):
    return ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
            '<s:Body><u:%sResponse xmlns:u="urn:schemas-upnp-org:service:WANIPConnection:1">%s'
            '</u:%sResponse></s:Body></s:Envelope>' % (action, inner, action))

def fault(code):
    return ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
            '<s:Body><s:Fault><detail><UPnPError xmlns="urn:schemas-upnp-org:control-1-0">'
            '<errorCode>%d</errorCode></UPnPError></detail></s:Fault></s:Body></s:Envelope>' % code)

def arg(body, name):
    m = re.search(r'<%s>(.*?)</%s>' % (name, name), body)
    return m.group(1) if m else ''

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass
    def send(self, code, body):
        b = body.encode()
        self.send_response(code)
        self.send_header('Content-Type', 'text/xml')
        self.send_header('Content-Length', str(len(b)))
        self.end_headers()
        self.wfile.write(b)
    def do_GET(self):
        log.write('GET %s\n' % self.path); log.flush()
        self.send(200, DESC.replace('<deviceType>', '<modelName>FRITZ!Box 7590</modelName><deviceType>', 1) if mode == 'fritz' else DESC)
    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length'])).decode()
        action = self.headers['SOAPAction'].strip('"').split('#')[1]
        log.write('SOAP %s %s\n' % (action, body[body.find('<u:'):][:400])); log.flush()
        if action == 'GetExternalIPAddress':
            ip = '100.64.12.34' if mode == 'cgnat' else '93.184.216.34'
            self.send(200, envelope(action, '<NewExternalIPAddress>%s</NewExternalIPAddress>' % ip))
        elif action == 'GetSpecificPortMappingEntry':
            port = int(arg(body, 'NewExternalPort'))
            if port in mappings:
                self.send(200, envelope(action, '<NewInternalClient>%s</NewInternalClient>' % mappings[port]))
            else:
                self.send(500, fault(714))
        elif action == 'AddPortMapping':
            port = int(arg(body, 'NewExternalPort'))
            if port == 31413:  # pretend another console already uses 31413
                self.send(500, fault(718)); return
            if mode == 'fritz':  # device not yet allowed in the FRITZ!Box
                self.send(500, fault(606)); return
            mappings[port] = arg(body, 'NewInternalClient')
            self.send(200, envelope(action, ''))
        elif action == 'DeletePortMapping':
            mappings.pop(int(arg(body, 'NewExternalPort')), None)
            self.send(200, envelope(action, ''))
        else:
            self.send(500, fault(401))

def ssdp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('', 1900))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                 struct.pack('4s4s', socket.inet_aton('239.255.255.250'), socket.inet_aton('0.0.0.0')))
    while True:
        data, addr = s.recvfrom(2048)
        if b'M-SEARCH' in data:
            log.write('SSDP from %s\n' % (addr,)); log.flush()
            s.sendto(('HTTP/1.1 200 OK\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n'
                      'LOCATION: http://127.0.0.1:%d/desc.xml\r\nUSN: uuid:test\r\n\r\n' % HTTP_PORT).encode(), addr)

def pcp():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(('127.0.0.1', 5351))
    while True:
        q, addr = s.recvfrom(1100)
        log.write('PCP %d bytes ver %d op %d\n' % (len(q), q[0], q[1])); log.flush()
        if len(q) == 60 and q[0] == 2 and q[1] == 1:
            r = bytearray(60)
            r[0], r[1], r[3] = 2, 0x81, 0
            r[4:8] = q[4:8]
            r[24:36] = q[24:36]
            r[36] = q[36]
            r[40:42] = q[40:42]
            r[42:44] = struct.pack('>H', 40001)
            r[44:60] = bytes(10) + b'\xff\xff' + socket.inet_aton('93.184.216.35')
            s.sendto(bytes(r), addr)

if mode in ('upnp', 'cgnat', 'fritz'):
    threading.Thread(target=ssdp, daemon=True).start()
    srv = http.server.ThreadingHTTPServer(('127.0.0.1', HTTP_PORT), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
else:
    threading.Thread(target=pcp, daemon=True).start()
print('ready', flush=True)
threading.Event().wait()
