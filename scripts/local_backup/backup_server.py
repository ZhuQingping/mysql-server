# coding: utf-8
###################
# Description: 
###################
#
#  `backup_server.py` is the server side of Control Scripts.
#  Sever would handle requests from client which is backup_ctrl.py
#  
#  There are 2 types requests for now:
#  * REQ_TRANSFER 
#  * REQ_BACKUP (TODO)


import socket
import os
import struct
from datetime import datetime
from backup_config import GLOBAL_CONFIG, config
import threading
import fnmatch

FILE_BLACKLIST = [
    '*.cnf',
    'tmp/*',
    'conf/*',
    'log/*',
    '#innodb_temp/*',
    'ibtmp*',
    '*.sock',
  #  '*.log',
]

def server_log(message):
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    with open("server.log", "a") as f:
        f.write("[%s] SERVER: %s\n" % (timestamp, message))

def recv_all(sock, n):
    data = ""
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data += packet
    return data

def should_skip_file(rel_path):
    unix_path = rel_path.replace(os.path.sep, '/')

    for pattern in FILE_BLACKLIST:
        if config.debug:
            server_log("Process-> check file unix_path: %s pattern: %s" % (unix_path, pattern))
        if fnmatch.fnmatchcase(unix_path, pattern):
            return True

        if '/' in pattern and fnmatch.fnmatchcase('/' + unix_path, '*/' + pattern):
            return True
    return False

def send_directory(conn, path):
    """Send directory which type is 0x02"""
    path_encoded = path.replace(os.path.sep, '/').encode("utf-8")
    header = struct.pack(">BH", 0x02, len(path_encoded))
    conn.sendall(header + path_encoded)
    server_log("Sent directory: %s" % path)

def send_file(conn, base_dir, filepath):
    """Send file which type is 0x03"""
    rel_path = os.path.relpath(filepath, base_dir).replace(os.path.sep, '/')
    try:
        file_size = os.path.getsize(filepath)
        filename_encoded = rel_path.encode("utf-8")
        header = struct.pack(">BHI", 0x03, len(filename_encoded), file_size)
        conn.sendall(header + filename_encoded)
        server_log("Starting to send: %s (%d bytes)" % (rel_path, file_size))

        sent_bytes = 0
        with open(filepath, "rb") as f:
            while sent_bytes < file_size:
                chunk = f.read(4096)
                if not chunk:
                    break
                conn.sendall(chunk)
                sent_bytes += len(chunk)
                # server_log("Sent %s: %d/%d" % (rel_path, sent_bytes, file_size))

        server_log("Finished sending: %s" % rel_path)
        return sent_bytes == file_size
    except Exception as e:
        server_log("File error: %s - %s" % (rel_path, str(e)))
        return False

def handle_command_ack(conn):
    server_log("Received command 0x01 (ACK)")
    conn.sendall(chr(0x01))  # 发送确认响应
    server_log("Sent ACK response")

def handle_command_transfer(conn):
    server_log("Received command 0x02 (File Transfer)")
    base_dir = config.base_dir
    server_log("Conf send directory: %s" % (base_dir))

    for root, dirs, files in os.walk(base_dir):
        # Handle directories
        rel_dir = os.path.relpath(root, base_dir).replace(os.path.sep, '/')
        if rel_dir == '.':
            rel_dir = ''
        else:
            dir_visible = True
            for pattern in FILE_BLACKLIST:
                server_log("Process-> blacklisted rel_dir: %s pattern: %s" % (rel_dir, pattern))
                if fnmatch.fnmatchcase(rel_dir.replace(os.path.sep, '/'), pattern):
                    server_log("Skipping blacklisted directory: %s" % (rel_dir))
                    dir_visible = False
                    break
            if not dir_visible:
                continue
            send_directory(conn, rel_dir)

        # Handle files
        for filename in files:
            filepath = os.path.join(root, filename)
            rel_path = os.path.relpath(filepath, base_dir).replace(os.path.sep, '/')

            if should_skip_file(rel_path):
                server_log("Skipping blacklisted file: %s" % (rel_path))
                continue
            send_file(conn, base_dir, filepath)

    conn.sendall(chr(0xFF))  # 传输结束标记
    server_log("File transfer completed")

def handle_client(conn, addr):
    try:
        server_log("Client connected from %s:%d" % (addr[0], addr[1]))

        cmd = recv_all(conn, 1)
        if not cmd:
            server_log("No command received")
            return

        cmd_type = ord(cmd[0])
        if cmd_type == 0x01:
            handle_command_ack(conn)
        elif cmd_type == 0x02:
            handle_command_transfer(conn)
        else:
            server_log("Invalid command: 0x%02X" % cmd_type)
            conn.sendall(chr(0xFF))

    except Exception as e:
        server_log("Error: %s" % str(e))
    finally:
        conn.close()
        server_log("Client disconnected: %s:%d" % (addr[0], addr[1]))

def run_server(host, port):
    server_log("Starting server on %s:%d" % (host, port))
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((host, port))
    sock.listen(5)

    try:
        while True:
            conn, addr = sock.accept()
            client_thread = threading.Thread(target=handle_client, args=(conn, addr))
            client_thread.start()
    except KeyboardInterrupt:
        server_log("Server shutting down")
    finally:
        sock.close()

if __name__ == "__main__":
    run_server(config.server_host, config.port)
