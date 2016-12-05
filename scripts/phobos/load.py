import os
import numpy as np
from phobos import cobs
from phobos import pose

script_dir = os.path.dirname(os.path.realpath(__file__))
project_dir = os.path.join(script_dir,
        os.path.pardir, os.path.pardir, 'projects')
flimnap_file = os.path.join(project_dir, 'flimnap', 'main.cc')

def pose_logfile(filename, dtype=None):
    if dtype is None:
        _, dtype, _ = pose.parse_format(flimnap_file)

    bytedata = None
    gitsha1 = None
    num_errors = 0
    data = bytearray()

    with open(filename, 'rb') as f:
        bytedata = f.read()

    packet_start = 0;
    packet_end = 0;
    mv = memoryview(bytedata)
    for i, byte in enumerate(bytedata):
        #if byte == b'\x00': # COBS packet delimiter
        if byte == 0: # b'\x00': # COBS packet delimiter
                      # Python 3.3+ returns an int when iterating over a
                      # 'bytes' object??
            packet_end = i
            try:
                p = cobs.decode(mv[packet_start:packet_end], True)
            except cobs.DecodeError as e:
                # further testing necessary to determine cause of:
                # 'not enough input bytes for length code'
                # print(e)
                num_errors += 1
                # add a packet with all floats as nan
                data.extend(bytearray([0xff] * dtype.itemsize))
            else:
                if len(p) != dtype.itemsize:
                    if len(p) == 7: # this is sent as the first packet
                        gitsha1 = p.decode('ascii')
                    else:
                        print('invalid packet size: {0} not {1}'.format(
                            len(p), dtype.itemsize))
                else:
                    data.extend(p)
            finally:
                packet_start = i + 1
                packet_end = i + 1

    return gitsha1, np.frombuffer(data, dtype), num_errors
