#
# Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#

import math
import subprocess
import os
import time
import socket
import six
import fcntl
import tempfile
import threading
import contextlib

# Code borrowed from http://wiki.python.org/moin/PythonDecoratorLibrary#Retry


def retry(tries=5, delay=3):
    '''Retries a function or method until it returns True.
    delay sets the initial delay in seconds.
    '''
    tries = tries * 1.0
    tries = math.floor(tries)
    if tries < 0:
        raise ValueError("tries must be 0 or greater")

    if delay <= 0:
        raise ValueError("delay must be greater than 0")

    def deco_retry(f):
        def f_retry(*args, **kwargs):
            mtries, mdelay = tries, delay  # make mutable

            rv = f(*args, **kwargs)  # first attempt
            while mtries > 0:
                if rv is True:  # Done on success
                    return True
                mtries -= 1      # consume an attempt
                time.sleep(mdelay)  # wait...

                rv = f(*args, **kwargs)  # Try again
            return False  # Ran out of tries :-(

        return f_retry  # true decorator -> decorated function
    return deco_retry  # @retry(arg[, ...]) -> true decorator
# end retry


def web_invoke(httplink):
    cmd = 'curl ' + httplink
    output = None
    try:
        output = subprocess.check_output(cmd, shell=True)
    except Exception:
        output = None
    return output
# end web_invoke


def obj_to_dict(obj):
    if isinstance(obj, dict):
        data = {}
        for k, v in obj.items():
            data[k] = obj_to_dict(v)
        return data
    elif hasattr(obj, '__iter__') and not isinstance(obj, six.string_types):
        return [obj_to_dict(v) for v in obj]
    elif hasattr(obj, '__dict__'):
        data = dict([(key, obj_to_dict(value)) 
            for key, value in list(obj.__dict__.items())
            if value is not None and not callable(value) and \
               not key.startswith('_')])
        return data
    else:
        return obj
# end obj_to_dict

def find_buildroot(path):
    try:
        return os.environ['BUILDTOP']
    except:
        return path + '/build/debug'
#end find_buildroot

def _get_free_port_naively():
    cs = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    cs.bind(("", 0))
    cport = cs.getsockname()[1]
    cs.close()
    return cport
#end _get_free_port_naively

free_port_guard = threading.Lock()
def get_free_port():
    output = 0
    x = 'ip_local_reserved_ports'
    P = '/proc/sys/net/ipv4/{0}'.format(x)
    if not os.path.exists(P):
        return _get_free_port_naively()

    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as s:
        s.bind(('', 0))
        output = s.getsockname()[1]
        with free_port_guard:
            F, p = tempfile.mkstemp()
            with os.fdopen(F, 'w') as T:
                g = '{0}/{1}.lck'.format(os.path.dirname(p), x)
                with open(g, 'w+') as G:
                    fcntl.lockf(G, fcntl.LOCK_EX)
                    with open(P, 'r') as f:
                        t = str(f.read(2**20)).strip()

                    if 0 < len(t):
                        t += ','

                    t += str(output)
                    T.write(t)
                    T.close()

                    e = os.system("sudo -n sh -c 'cat {0} > {1}'".format(p, P))
                    os.remove(p)
                    if 0 != e:
                        raise Exception("Cannot write to %s: %d" % (P, e))

    return output
#end get_free_port
