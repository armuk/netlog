#ifndef __NETLOG__
#define __NETLOG__

/* Change to non zero value (i.e. 1) if you wish to probe 
 * the binding of UDP sockets.
 */

#define PROBE_UDP 0

/* Change to zero value (0) if you wish to not probe 
 * the close system call for the sockets.
 */

#define PROBE_CONNECTION_CLOSE 1

/* Set to non-zero value in order to compile the whitelisting code*/

#define WHITELISTING 1

/* Error codes */

#define CONNECT_PROBE_FAILED -1
#define ACCEPT_PROBE_FAILED -2
#define CLOSE_PROBE_FAILED -3
#define BIND_PROBE_FAILED -4 
#define LOG_FAILURE -5

/* Max lenght of the execution path of the process to be whitelisted.
 * Must be less or equal to MAX_ABSOLUTE_EXEC_PATH, defined in the
 * whitelist.h header file.
 */

#define MAX_EXEC_PATH 64

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Panos Sakkos <panos.sakkos@cern.ch>");
MODULE_DESCRIPTION("netlog logs information about every internet connection\
		 from and to the machine that is installed. This information\
		  is source/destination ips and ports, process name and pid, \
		  uid and the protocol (TCP/UDP).");

int __init plant_probes(void);
void __exit unplant_probes(void);

module_init(plant_probes);
module_exit(unplant_probes);

#endif
