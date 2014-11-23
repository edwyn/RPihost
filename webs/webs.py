import socket
import cgi
import BaseHTTPServer

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server_address = ('localhost', 8888)
sock.connect(server_address)

class MyHandler( BaseHTTPServer.BaseHTTPRequestHandler ):
    server_version= "MyHandler/1.1"
    def do_GET( self ):
        self.log_message( "Command: %s Path: %s Headers: %r"
                          % ( self.command, self.path, self.headers.items() ) )
        self.fbi_repeat()
        self.do_upload()
        self.dumpReq( None )
    def do_POST( self ):
        form = cgi.FieldStorage(
            fp=self.rfile,
            headers=self.headers,
            environ={'REQUEST_METHOD':'POST',
                     'CONTENT_TYPE':self.headers['Content-Type'],
                     })
        filename = form['upfile'].filename
        data = form['upfile'].file.read()
        open("/home/pi/%s"%filename, "wb").write(data)
        self.dumpReq( None )
    def dumpReq( self, formInput=None ):
        response= "<html><head></head><body>"
        response+= "<p>HTTP Request</p>"
        response+= "<p>self.command= <tt>%s</tt></p>" % ( self.command )
        response+= "<p>self.path= <tt>%s</tt></p>" % ( self.path )
        response+= "</body></html>"
        self.sendPage( "text/html", response )
    def sendPage( self, type, body ):
        self.send_response( 200 )
        self.send_header( "Content-type", type )
        self.send_header( "Content-length", str(len(body)) )
        self.end_headers()
        self.wfile.write( body )
    def fbi_repeat(self):
        if self.path.endswith("go"):
            try:
                message = 'r'
                sock.sendall(message)
                amount_received = 0
                amount_expected = len(message)
                while amount_received < amount_expected:
                    data = sock.recv(16)
                    amount_received += len(data)
            finally:
                self.log_message("finished sending repease")
    def do_upload(self):
        if self.path.endswith("upload"):
            response= "<html><head></head><body>"
            response+= "<form method='POST' enctype='multipart/form-data'>"
            response+= "File to upload: <input type=file name=upfile><br>"
            response+= "<br><input type=submit value=Press> to upload the file!"
            response+= "</form></body></html>"
            self.sendPage( "text/html", response )


def httpd(handler_class=MyHandler, server_address = ('', 8088), ):
    srvr = BaseHTTPServer.HTTPServer(server_address, handler_class)
    srvr.serve_forever()
    #srvr.handle_request() # serve_forever

if __name__ == "__main__":
    httpd( )
