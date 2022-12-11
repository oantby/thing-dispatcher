import os
import socket
import time
import atexit

SERVER_SOCKET = '/tmp/stuff_dispatcher.sock'
SOCKET_PATH = '/tmp/dispclient_%d.%d.sock' % (os.getpid(), time.time())

My_Socket = None
def connect():
	global My_Socket
	My_Socket = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM, 0)
	My_Socket.bind(SOCKET_PATH)
	My_Socket.settimeout(1)
	atexit.register(rm_socket)

def rm_socket():
	global My_Socket
	My_Socket.close()
	os.remove(SOCKET_PATH)
	My_Socket = None

def send_request(req):
	global My_Socket
	if My_Socket is None:
		connect()
	if type(req) == str:
		req = req.encode()
	
	My_Socket.sendto(req, SERVER_SOCKET)
	try:
		return My_Socket.recv(1023).decode()
	except TimeoutError:
		return None

