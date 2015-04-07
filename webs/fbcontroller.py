#/usr/bin/env python
from threading import Thread
import socket
import time
import sys

class FBController():
    def __init__(self,port=None):
        self.data=None #received data storage
        self.port=None
        self.socket=None # socket connection connected to Framebuffer service
        self.server_address=None # server address to Framebuffer service

        if port is not None:
            self.connect(port)


    def disconnect(self):
        """Disconnects from Framebuffer service 
        """
        if(self.socket):
            self.socket.close()
        self.socket=None

    def connect(self,port=None):
        """Set port and connect to framebuffer service
        """
        if(self.socket):
            self.disconnect()
        if port is not None:
            self.port=port
        if self.server_address is None:
            self.server_address = ('localhost', self.port)

        if self.port is not None:
            # Create a TCP/IP socket
            self.socket=socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.connect(self.server_address)


    def send(self,command):
        self.socket.sendall(command)
        # Look for the response
        amount_received = 0
        amount_expected = len(message)
        while amount_received < amount_expected:
            data = sock.recv(16)
            amount_received += len(data)
        self.data = data

