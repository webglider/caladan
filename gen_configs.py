# Usage: python gen_configs.py <num-lapps> <lapp-bcores> <lapp-gcores> <lapp-qdelay> <num-tapps> <tapp-bcores>

import sys

num_lapps = int(sys.argv[1])
lapp_bcores = int(sys.argv[2])
lapp_gcores = int(sys.argv[3])
lapp_qdelay = int(sys.argv[4])

num_tapps = int(sys.argv[5])
tapp_bcores = int(sys.argv[6])

lapp_config = """host_addr 192.168.10.%d
host_netmask 255.255.255.0
host_gateway 192.168.10.1
runtime_kthreads %d
runtime_guaranteed_kthreads %d
#runtime_spinning_kthreads 1
enable_directpath 1
runtime_priority lc
runtime_qdelay_us %d
"""

tapp_config = """host_addr 192.168.10.%d
host_netmask 255.255.255.0
host_gateway 192.168.10.1
runtime_kthreads %d
runtime_guaranteed_kthreads 0
#runtime_spinning_kthreads 1
runtime_priority be
enable_directpath 1
"""

for i in range(1, num_lapps+1):
    f = open('lat%d.config'%(i), 'w')
    f.write(lapp_config % (50 + i, lapp_bcores, lapp_gcores, lapp_qdelay))
    f.close()

for i in range(1, num_tapps+1):
    f = open('thru%d.config'%(i), 'w')
    f.write(tapp_config % (10 + i, tapp_bcores))
    f.close()

print('Config files written')