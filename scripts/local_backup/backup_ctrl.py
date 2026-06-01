# coding: utf-8
######################
# Description
#####################
# backup_ctrl.py is the ctrl client tool

import socket
import os
import re
import struct
import sys
import json
import subprocess
import shutil
#from MySQLdb import connect, OperationalError
from datetime import datetime
from backup_config import GLOBAL_CONFIG, config

def client_log(message):
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    with open("client.log", "a") as f:
        f.write("[%s] CLIENT: %s\n" % (timestamp, message))

def recv_all(sock, n):
    data = ""
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data += packet
    return data

def process_pathectory(base_path, path):
    full_path = os.path.join(base_path, path)
    if config.debug:
        client_log("Process-> directory: %s" % full_path)
    try:
        if not os.path.exists(full_path):
            os.makedirs(full_path)
            client_log("Created directory: %s" % full_path)
        else:
            client_log("Skip created directory: %s" % full_path)
    except OSError as e:
        if e.errno != 17:
            client_log("Directory error: %s" % str(e))

def process_file(base_path, filename, file_size, sock):
    full_path = os.path.join(base_path, filename)
    if config.debug:
        client_log("Process-> file: %s" % full_path)
    if not os.path.exists(full_path):
        try:
            os.makedirs(os.path.dirname(full_path))
        except OSError:
            pass

    received = 0
    with open(full_path, "wb") as f:
        while received < file_size:
            chunk_size = min(4096, file_size - received)
            chunk = recv_all(sock, chunk_size)
            if not chunk:
                break
            f.write(chunk)
            received += len(chunk)
            # client_log("Received %s: %d/%d" % (filename, received, file_size))
    client_log("Received %s (%d/%d bytes)" % (filename, received, file_size))

    status = received == file_size
    log_msg = "%s (%d/%d bytes)" % (filename, received, file_size)
    if status:
        client_log("Success: " + log_msg)
    else:
        client_log("Failed: " + log_msg)
    return status

def copy_src_to_dest_no_delete(source, destination):
    """
    Copy files/directories while preserving destination structure and existing files
    Returns: (success, errors)
             success: Boolean indicating overall success
             errors: List of tuples (path, error_message)
    """
    errors = []
    success = True

    try:
        # Validate source existence
        if not os.path.exists(source):
            raise ValueError("Source path does not exist: {}".format(source))

        # Normalize paths
        source = os.path.normpath(source)
        destination = os.path.normpath(destination)

        # Handle file copy
        if os.path.isfile(source):
            dest_dir = os.path.dirname(destination)

            # Create destination directory if needed
            if not os.path.exists(dest_dir):
                try:
                    os.makedirs(dest_dir)
                except OSError as e:
                    errors.append((source, "Directory creation failed: {}".format(e)))
                    return (False, errors)

            # Perform file copy with overwrite
            try:
                shutil.copy2(source, destination)
                return (True, errors)
            except Exception as e:
                errors.append((source, "File copy failed: {}".format(e)))
                return (False, errors)

        # Handle directory copy (BFS non-recursive approach)
        if os.path.isdir(source):
            queue = collections.deque()
            queue.append((source, destination))

            while queue:
                src_path, dest_path = queue.popleft()

                # Create destination directory
                if not os.path.exists(dest_path):
                    try:
                        os.makedirs(dest_path)
                    except OSError as e:
                        errors.append((src_path, "Directory creation failed: {}".format(e)))
                        success = False
                        continue

                # Process directory contents
                try:
                    entries = os.listdir(src_path)
                except OSError as e:
                    errors.append((src_path, "Directory listing failed: {}".format(e)))
                    success = False
                    continue

                for entry in entries:
                    src_entry = os.path.join(src_path, entry)
                    dest_entry = os.path.join(dest_path, entry)

                    if os.path.isdir(src_entry):
                        # Add directory to processing queue
                        queue.append((src_entry, dest_entry))
                    else:
                        # Copy file with overwrite
                        try:
                            shutil.copy2(src_entry, dest_entry)
                            client_log("Copy file done, from [%s] to [%s]" % (src_entry, dest_entry))
                        except Exception as e:
                            errors.append((src_entry, "File copy failed: {}".format(e)))
                            success = False
            return (success, errors)

    except Exception as e:
        errors.append((source, "Critical error: {}".format(e)))
        return (False, errors)

def copy_src_to_dest(src, dest):
    """
    Copy files/directories to destination but with delete target directory first
    """
    try:
        src = os.path.abspath(src)
        dest = os.path.abspath(dest)

        #client_log("Copy file function begin, [%s] to [%s]" % (src, dest))
        if not os.path.exists(src):
            raise ValueError("Source Path not exist: %s" % (src))

        if os.path.isdir(src):
            base_name = os.path.basename(src)
            target_path = os.path.join(dest, base_name)

            if os.path.exists(target_path):
                shutil.rmtree(target_path)

            shutil.copytree(src, target_path)
            client_log("    Copy directory done, from [%s] to [%s]" % (src, target_path))

        elif os.path.isfile(src):
            if os.path.isdir(dest):
                target_file = os.path.join(dest, os.path.basename(src))
            else:
                target_file = dest
                #client_log("Copy file dest is file, from [%s] to [%s]" % (src, target_file))
                parent_path = os.path.dirname(target_file)
                if not os.path.exists(parent_path):
                    os.makedirs(parent_path)

            shutil.copy2(src, target_file)
            client_log("    Copy file done, from [%s] to [%s]" % (src, target_file))
        else:
            raise ValueError("Invalid source type, %s" % (src))
    except Exception as e:
        client_log("[ERROR] copy failed, %s" % (str(e)))
        raise RuntimeError("Copy failed from %s to %s, %s" % (src, dest, str(e)))

def copy_file_chunk(src, dest, chunk_size):
    """
    Copy file to destination with specific chunk size.
    """
    try:
        src = os.path.abspath(src)
        dest = os.path.abspath(dest)

        if not os.path.exists(src):
            raise ValueError("Source Path not exist: %s" % (src))

        parent_path = os.path.dirname(dest)
        if not os.path.exists(parent_path):
            os.makedirs(parent_path)

        with open(src, 'rb') as src_file:
            with open(dest, 'wb') as dest_file:
                chunk = src_file.read(chunk_size)
                if not chunk:
                    raise ValueError("Invalid chunk size: %s" % (chunk_size))
                dest_file.write(chunk)

        client_log("    Copy file done, from [%s] to [%s]" % (src, dest))
    except Exception as e:
        client_log("[ERROR] copy failed, %s" % (str(e)))
        raise RuntimeError("Copy failed from %s to %s, %s" % (src, dest, str(e)))

def find_files_with_prefix(directory, prefix):
    matched_files = []
    for root, dirs, files in os.walk(directory):
        for filename in files:
            if filename.startswith(prefix):
                full_path = os.path.join(root, filename)
                matched_files.append(full_path)
    return matched_files

def copy_files_to_output(json_data):
    target_path = config.backup_output_path
    shutil.rmtree(target_path)
    os.mkdir(target_path)
    client_log("[LocalBackup] Step 2. begin copy files to %s" % (target_path))
    try:
        # Copy full local backup dir.
        src_path = os.path.join(config.backup_data_path, json_data.get("backupDir"))
        copy_src_to_dest(src_path, target_path)

        # Copy wal files.
        for wal_file in json_data.get("walFileList"):
            target_file = os.path.join(config.backup_output_path, os.path.basename(wal_file))
            if wal_file == json_data.get("walFileList")[-1]:
                chunk_size = json_data.get("lastWalFileSize")
                copy_file_chunk(wal_file, target_file, chunk_size)
            else:
                copy_src_to_dest(wal_file, target_file)

        # Copy restore meta.
        restore_meta_timestamp = datetime.strptime(json_data.get("timePointStr"), "%Y-%m-%d %H:%M:%S")
        restore_meta_timestamp_str = restore_meta_timestamp.strftime("%YY%mM%dD%HH%MM%SS")
        restore_meta_filename = "RestoreMeta_" + restore_meta_timestamp_str

        restore_meta = os.path.join(config.backup_meta_path, restore_meta_filename)
        if not os.path.exists(restore_meta):
            raise RuntimeError("could not find the restore meta in %s" % (config.backup_meta_path))
            return 1
        target_restore_meta = os.path.join(config.backup_output_path, os.path.basename(restore_meta))
        copy_src_to_dest(restore_meta, target_restore_meta)
        return 0
    except OSError as e:
        if e.errno != 17:
            client_log("[LocalBackup] copy backup files error: %s" % str(e))
        return 1
    except RuntimeError as e:
        client_log("[LocalBackup] copy backup files error: %s" % (str(e)))
        return 1

def run_create_data(timepoint):
    """
    run `backup_monitor -t` to get the result json, and copy to data directory
    """
    try:
        client_log("[LocalBackup] Run LocalBackup \"%s\"" % (timepoint))
        command = [config.backup_monitor, config.backup_meta_path]
        command.append('-t')
        command.append(timepoint)
        command.append('-s')
        command.append(config.wal_file_size)

        p = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=None, stdin=subprocess.PIPE)
        print (command)
        stdout, stderr = p.communicate()
        if p.returncode != 0:
            return p.returncode

        json_data = json.loads(stdout)
        if config.debug:
            print(json.dumps(json_data, indent=2))
            client_log("[LocalBackup] Step 1. run backup monitor to get file list done, json:\n%s" % (json.dumps(json_data, indent=2)))

        return copy_files_to_output(json_data)
    except subprocess.CalledProcessError as e:
        client_log("[LocalBackup] run command failed, %d, %s" % (e.returncode, e.output))
        return 2
    except OSError as e:
        client_log("OSError, %s" % (str(e)))
        return 3
    except ValueError as e:
        client_log("Parse Json failed, %s" % (str(e)))
        client_log("Original:\n %s" % (output))
        return 4

def run_list_time_point(arg):
    """
    run `backup_monitor -l/-w` to list the time point to time_point_list file.
    """
    try:
        command = [config.backup_monitor, config.backup_meta_path]
        if arg == 'full_backup':
            client_log("[LocalBackup] List time point based on full backup meta.")
            command.append('-l')
            command.append('-d')
            command.append('full_backup_time_point_list')
        elif arg == 'wal_archive':
            client_log("[LocalBackup] List time point based on wal archive meta.")
            command.append('-w')
            command.append('-d')
            command.append('wal_archive_time_point_list')

        p = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=None, stdin=subprocess.PIPE)
        print (command)
        stdout, stderr = p.communicate()
        if p.returncode != 0:
            return p.returncode
        return 0
    except RuntimeError as e:
        client_log("[LocalBackup] list time point error: %s" % (str(e)))
        return 1
    except subprocess.CalledProcessError as e:
        client_log("[LocalBackup] run command failed, %d, %s" % (e.returncode, e.output))
        return 2
    except OSError as e:
        client_log("OSError, %s" % (str(e)))
        return 3
    except ValueError as e:
        client_log("Parse Json failed, %s" % (str(e)))
        client_log("Original:\n %s" % (output))
        return 4

def run_copy_back():
    """
    copy from backup output data to data directory.
    there are 3 kinds files:
        - full_backup_data
        - multiple wal files which are log archived
        - RestoreMeta
    """
    src_path = config.backup_output_path
    copy_back_data_path = config.copy_back_data_path
    copy_back_data_path = os.path.normpath(copy_back_data_path)
    copy_back_meta_path = config.copy_back_meta_path
    copy_back_meta_path = os.path.normpath(copy_back_meta_path)
    client_log("[LocalBackup] Step 4. begin copy back from %s to %s and %s" % (src_path, copy_back_data_path, copy_back_meta_path))
    try:
        # move target_path to target_path_old
        timestamp = datetime.now().strftime("%Y-%m-%d_%H:%M:%S")
        copy_back_data_tmp_path = copy_back_data_path + "_old_" + timestamp
        if not os.path.exists(copy_back_data_path):
            raise RuntimeError("copy_back_data_path not exists")

        shutil.move(copy_back_data_path, copy_back_data_tmp_path)
        client_log("    Move target directory done, from [%s] to [%s]" % (copy_back_data_path, copy_back_data_tmp_path))

        # handle full backup directory, eg: 2025Y04M17D20H12M53S
        full_backup_pattern = r'^\d{4}Y\d{2}M\d{2}D\d{2}H\d{2}M\d{2}S'
        full_backup_regex = re.compile(full_backup_pattern)

        wal_archive_pattern = r'^[0-9A-Fa-f]{8}_[0-9A-Fa-f]{8}_[0-9A-Fa-f]{16}'
        wal_archive_regex = re.compile(wal_archive_pattern)
        restore_meta_prefix = "RestoreMeta_"
        full_backup_file_flag = False
        restore_meta_file_flag = False
        wal_archive_file_flag = False
        pdb_path = "#mysql_dstore/gs_pdb/"
        if config.use_default_template_pdb:
            pdb_path += "test_tenant.vfs.template1/"
        else:
            pdb_path += "PDB_17/"

        for item in os.listdir(src_path):
            item_path = os.path.join(src_path, item)
            client_log("    found file %s" % (item_path))

            if os.path.isfile(item_path):
                # handle the RestoreMeta, eg: RestoreMeta_2025Y04M17D20H12M53S
                if item.startswith(restore_meta_prefix):
                    target_restore_meta = os.path.join(copy_back_meta_path, "RestoreMeta_RestoreMeta")
                    copy_src_to_dest(item_path, target_restore_meta)
                    restore_meta_file_flag = True
            elif os.path.isdir(item_path):
                if full_backup_regex.match(item):
                    client_log("   -> handle full backup data, from [%s] to [%s]" %
                            (item_path, copy_back_data_path))
                    shutil.copytree(item_path, copy_back_data_path)
                    client_log("    <- Copy full backup dstore done")

                    ## handle some files which are not backuped:
                    files_to_copy = [
                        ["#mysql_dstore/gs_pdb/test_tenant.vfs.root_pdb", True],
                        ["#mysql_dstore/gs_pdb/test_tenant.vfs.runtime_log", True],
                        ["#mysql_dstore/gs_pdb/test_tenant.vfs.template0", True],
                        ["#mysql_dstore/gs_pdb/test_tenant.vfs.voting_disk", True],
                        [pdb_path + "decodedict_file_1", False],
                        [pdb_path + "decodedict_file_2", False],
                        ["asp_data", False],
                        ["gs_profile", False],
                        ["auto.cnf", False],
                        ["client-cert.pem", False],
                        ["client-key.pem", False],
                        ["private_key.pem", False],
                        ["public_key.pem", False],
                        ["server-cert.pem", False],
                        ["server-key.pem", False],
                        ["sql_monitor", False],
                        ["ib_buffer_pool", False]
                    ]

                    for filename in files_to_copy:
                        src_file = os.path.join(copy_back_data_tmp_path, filename[0])
                        dst_file = os.path.join(copy_back_data_path, filename[0])
                        must_exist = filename[1]

                        if os.path.exists(src_file):
                            copy_src_to_dest(src_file, dst_file)
                            client_log("    Copy backup old file done, from [%s] to [%s]" %
                                (src_file, dst_file))
                        elif not must_exist:
                            client_log("    Copy backup old file not exist: %s" % (src_file))
                        else:
                            raise RuntimeError("    copy back file not exit, %s" % (src_file))

                    full_backup_file_flag = True

        for item in os.listdir(src_path):
            item_path = os.path.join(src_path, item)
            if os.path.isfile(item_path) and wal_archive_regex.match(item):
                # handle wal files, eg: 00000001_00000000_0000000000000000
                wal_file_path = os.path.join(copy_back_data_path, pdb_path + "wal/")
                copy_src_to_dest(item_path, wal_file_path)
                wal_archive_file_flag = True
        return True
    except OSError as e:
        if e.errno != 17:
            client_log("[LocalBackup] copy backup files error: %s" % str(e))
        return False
    except RuntimeError as e:
        client_log("[LocalBackup] copy backup files error: %s" % (str(e)))
        return False

def handle_ack_response(sock):
    response = recv_all(sock, 1)
    if response and ord(response[0]) == 0x01:
        client_log("Received ACK confirmation")
        return True
    client_log("Invalid ACK response")
    return False

def handle_file_transfer(sock):
    base_path = config.recv_path
    client_log("Conf recv_pathectory: %s" % (base_path))
    #os.makedirs(base_path, exist_ok=True)

    while True:
        header = recv_all(sock, 1)
        if not header:
            break

        cmd_type = ord(header[0])
        if cmd_type == 0x02:  # directory 
            data = recv_all(sock, 2)
            dirname_len = struct.unpack(">H", data)[0]
            dirname = recv_all(sock, dirname_len).decode("utf-8")
            process_pathectory(base_path, dirname)
        elif cmd_type == 0x03:  # file
            data = recv_all(sock, 6)
            filename_len, file_size = struct.unpack(">HI", data)
            filename = recv_all(sock, filename_len).decode("utf-8")
            process_file(base_path, filename, file_size, sock)
        elif cmd_type == 0xFF:  # EndFlag 
            client_log("Transfer completed")
            break
        else:
            client_log("Unknown command: 0x%02X" % cmd_type)

def send_command(sock, cmd_type):
    sock.sendall(chr(cmd_type))
    client_log("Sent command 0x%02X" % cmd_type)

    if cmd_type == 0x01:
        return handle_ack_response(sock)
    elif cmd_type == 0x02:
        handle_file_transfer(sock)
        return True
    return False

def run_client(host, port, command_type):
    client_log("Connecting to %s:%d" % (host, port))
    sock = socket.socket()
    try:
        sock.connect((host, port))
        if command_type == "test":
            success = send_command(sock, 0x01)
            client_log("Connection test %s" % ("success" if success else "failed"))
        elif command_type == "transfer":
            send_command(sock, 0x02)
    except Exception as e:
        client_log("Error: %s" % str(e))
    finally:
        sock.close()
        client_log("Disconnected from server")

def usage():
    print("Usage:")
    print("   python backup_ctrl.py test")
    print("   python backup_ctrl.py list_time_point full_backup")
    print("   python backup_ctrl.py list_time_point wal_archive")
    print("   python backup_ctrl.py create_data timestamp")
    print("   python backup_ctrl.py transfer")
    print("   python backup_ctrl.py copy_back")
    print("Example:")
    print("   python backup_ctrl.py create_data 1745053906")
    print ("Note that config.json is needed!")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        usage()
        sys.exit(1)

    command = sys.argv[1]
    exit_code = 0

    try:
        if command == 'list_time_point':
            if len(sys.argv) != 3:
                print("Error: a list type is needed!")
                usage()
                exit_code = 1
            else:
                exit_code = run_list_time_point(sys.argv[2])
        elif command == 'create_data':
            if len(sys.argv) != 3:
                print("Error: a time point is needed!")
                usage()
                exit_code = 1
            else:
                exit_code = run_create_data(sys.argv[2])
        elif command == 'transfer':
           exit_code = run_client(config.server_host, config.port, sys.argv[1])
        elif command == 'copy_back':
           exit_code = run_copy_back()
        else:
            print("Unknow command: {}".format(command))
            exit_code = 1
    except KeyboardInterrupt:
        print("\nQuit")
        exit_code = 255
