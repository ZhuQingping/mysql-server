# coding: utf-8
######################
# Description
#####################
# backup_config.py
# Usage: 
#   print(GLOBAL_CONFIG['host'])
#   print(config.host)

import json

GLOBAL_CONFIG = {
    "bind_host": "0.0.0.0",
    "server_host": "127.0.0.1",
    "port": 9999,
    "base_path": "/home/ci/run/master",
    "recv_path": "/home/ci/run/recv_data",
    "debug": True,
    "timeout": 30.0,
    "backup_monitor": "/home/ci/install/bin/backup_monitor",
    "backup_data_path": "/home/ci/run/local_backup/full_local_backup",
    "backup_meta_path": "/home/ci/run/local_backup/local_backup_meta",
    "backup_wal_path": "/home/ci/run/local_backup/wal_archive",
    "backup_output_path": "/home/ci/run/local_backup/backup_output",
    "copy_back_data_path": "/home/ci/run/replica/data/",
    # dstore_restore_meta_path
    "copy_back_meta_path": "/home/ci/run/replica/local_backup/local_backup_meta",
    "use_default_template_pdb": False,
    "wal_file_size": "134217728"
}

def load_config(config_file="config.json"):
    global GLOBAL_CONFIG

    try:
        with open(config_file) as f:
            file_config = json.load(f)
            GLOBAL_CONFIG.update(file_config)
    except IOError:
        print("[WARN] Config file not found, using defaults")
    except ValueError as e:
        print("[ERROR] Invalid JSON format: %s" % str(e))
    except Exception as e:
        print("[ERROR] Config loading failed: %s" % str(e))

load_config()

class _Config(object):
    def __getattr__(self, name):
        return GLOBAL_CONFIG.get(name, None)

config = _Config()

