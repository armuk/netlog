#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/init.h>
#include <linux/in.h>
#include <linux/net.h>
#include <net/ip.h>
#include <linux/socket.h>
#include <linux/version.h>
#include <linux/file.h>
#include <linux/unistd.h>
#include <linux/syscalls.h>
#include <linux/kallsyms.h>
#include "iputils.h"
#include "whitelist.h"
#include "logger.h"
#include "netlog.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 29)
	#define CURRENT_UID current->uid
#else
	#define CURRENT_UID current_uid()
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 0, 25)
	#define SPORT sport
#else
	#define SPORT inet_sport
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 0, 25)
	#define DPORT dport
#else
	#define DPORT inet_dport
#endif

#define MODULE_NAME "netlog: "

/* The next two probes are for the connect system call. We need to associate the process that 
 * requested the connection with the socket file descriptor that the kernel returned.
 * The socket file descriptor is available only after the system call returns. 
 * Though we need to be able to get the pointer to the socket struct that was given as a parameter
 * to connect and log its contents. We cannot have a process requesting two connects in the same time,
 * because when a system call is called, the process is suspended untill its end of execution.
 */

static struct socket *match_socket[PID_MAX_LIMIT] = {NULL};

static int netlog_inet_stream_connect(struct socket *sock, struct sockaddr *addr, int addr_len, int flags)
{	
	match_socket[current->pid] = sock;

	jprobe_return();
	return 0;
}

static int post_connect(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	int log_status;
	struct socket *sock;

	sock = match_socket[current->pid];
	
	if(sock == NULL || sock->sk == NULL)
	{
		goto out;
	}

	if((sock->sk->sk_family != AF_INET && sock->sk->sk_family != AF_INET6) || sock->sk->sk_protocol != IPPROTO_TCP)
	{
		goto out;
	}

	#if WHITELISTING

	if(is_whitelisted(current))
	{
		goto out;
	}

	#endif

	log_status = log_message("%s[%d] TCP %s:%d -> %s:%d (uid=%d)\n", current->comm, current->pid, 
						get_local_ip(sock), ntohs(inet_sk(sock->sk)->SPORT),
						get_remote_ip(sock), ntohs(inet_sk(sock->sk)->DPORT), 
						CURRENT_UID);

	if(LOG_FAILED(log_status))
	{
		printk(KERN_ERR MODULE_NAME "Failed to log message\n");		
	}

out:
	match_socket[current->pid] = NULL;
	return 0;
}

/* post_accept probe is called right after the accept system call returns.
 * In the return register is placed the socket file descriptor. So with the
 * user of regs_register_status we can get the socket file descriptor and log
 * the data that we want for the socket.
 */

static int post_accept(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct socket *sock;
	int err = 0, log_status;

	sock = sockfd_lookup(regs_return_value(regs), &err);

	if(sock == NULL ||sock->sk == NULL || err < 0)
	{
		goto out;
	}

	if((sock->sk->sk_family != AF_INET && sock->sk->sk_family != AF_INET6) || sock->sk->sk_protocol != IPPROTO_TCP)
	{
		goto out;
	}

	#if WHITELISTING

	if(is_whitelisted(current))
	{
		goto out;
	}

	#endif

	log_status = log_message("%s[%d] TCP %s:%d <- %s:%d (uid=%d)\n", current->comm, current->pid, 
						get_local_ip(sock), ntohs(inet_sk(sock->sk)->SPORT),
						get_remote_ip(sock), ntohs(inet_sk(sock->sk)->DPORT), 
						CURRENT_UID);
	if(LOG_FAILED(log_status))
	{
		printk(KERN_ERR MODULE_NAME "Failed to log message\n");	
	}

out:
	if(sock != NULL)
	{
		sockfd_put(sock);
	}

	return 0;
}

#if PROBE_CONNECTION_CLOSE

asmlinkage long netlog_sys_close(unsigned int fd)
{
	struct socket * sock;
	int log_status, err = 0;

	sock = sockfd_lookup(fd, &err);

	if(sock == NULL || sock->sk == NULL || err < 0)
	{
		goto out;
	}

	if(sock->sk->sk_protocol == IPPROTO_TCP && ntohs(inet_sk(sock->sk)->DPORT) != 0)
	{
		#if WHITELISTING

		if(is_whitelisted(current))
		{
			goto out;
		}

		#endif

		log_status = log_message("%s[%d] TCP %s:%d <-> %s:%d (uid=%d)\n", current->comm, current->pid, 
							get_local_ip(sock), ntohs(inet_sk(sock->sk)->SPORT),
							get_remote_ip(sock), ntohs(inet_sk(sock->sk)->DPORT), 
							CURRENT_UID);

		if(LOG_FAILED(log_status))
		{
			printk(KERN_ERR MODULE_NAME "Failed to log message\n");	
		}
	}
	#if PROBE_UDP
	if(sock->sk->sk_protocol == IPPROTO_UDP && ntohs(inet_sk(sock->sk)->DPORT) != 0)
	{
		#if WHITELISTING

		if(is_whitelisted(current))
		{
			goto out;
		}

		#endif

		log_status = log_message("%s[%d] UDP %s:%d <-> %s:%d (uid=%d)\n", current->comm, current->pid, 
							get_local_ip(sock), ntohs(inet_sk(sock->sk)->SPORT),
							get_remote_ip(sock), ntohs(inet_sk(sock->sk)->DPORT), 
							CURRENT_UID);

		if(LOG_FAILED(log_status))
		{
			printk(KERN_ERR MODULE_NAME "Failed to log message\n");		
		}
	}	
	#endif
	else
	{
		goto out;
	}
out:
	if(sock != NULL)
	{
		sockfd_put(sock);
	}
	
	jprobe_return();
	return 0;
}

#endif

#if PROBE_UDP

/* UDP protocol is connectionless protocol, so we probe the bind system call */

static int netlog_sys_bind(int sockfd, const struct sockaddr *addr, int addrlen)
{
	char *ip;
	struct socket * sock;
	int log_status, err = 0;

	sock = sockfd_lookup(sockfd, &err);

	if(sock == NULL || sock->sk == NULL || err < 0)
	{
		goto out;
	}

	if((sock->sk->sk_family != AF_INET && sock->sk->sk_family != AF_INET6) || sock->sk->sk_protocol != IPPROTO_UDP)
	{
		goto out;
	}

	#if WHITELISTING

	if(is_whitelisted(current))
	{
		goto out;
	}

	#endif

	ip = get_ip(addr);

	if(any_ip_address(ip))
	{
		log_status = log_message("%s[%d] UDP bind (any IP address):%d (uid=%d)\n", current->comm, 
					current->pid, ntohs(((struct sockaddr_in *)addr)->sin_port), CURRENT_UID);
	}
	else
	{
		log_status = log_message("%s[%d] UDP bind %s:%d (uid=%d)\n", current->comm,
					current->pid, ip, ntohs(((struct sockaddr_in6 *)addr)->sin6_port), CURRENT_UID);
	}

	if(LOG_FAILED(log_status))
	{
		printk(KERN_ERR MODULE_NAME "Failed to log message\n");	
	}

out:
	if(sock != NULL)
	{
		sockfd_put(sock);
	}

	jprobe_return();
	return 0;
}

#endif

int signal_that_will_cause_exit_with_preempt(int trap_number)
{
	printk(KERN_DEBUG "netlog: interrupt %d\n", trap_number);

	switch(trap_number)
	{
		case SIGABRT:
		case SIGSEGV:
		case SIGQUIT:
		//TODO Other signals that we need to handle?
			return 1;
			break;
		default:
			return 0;
			break;
	}
}

int handler_fault(struct kprobe *p, struct pt_regs *regs, int trap_number)
{
	/* In case of an interrupt that will cause the process to terminate,
	 * check if the preeemp_count is greater than 0 and decrease it by one,
	 * because it will not be decreased by kprobes.
	 */

	printk(KERN_DEBUG MODULE_NAME "fault handler: trap %d\n", trap_number);

	if(preempt_count() > 0  && signal_that_will_cause_exit_with_preempt(trap_number))
	{
		printk(KERN_DEBUG MODULE_NAME "fault handler: detected trap that will force the process to quit. Decreasing preempt_count\n");
		dec_preempt_count();
	}

	return 0;
}

/*************************************/
/*         probe definitions        */
/*************************************/

static struct jprobe connect_jprobe = 
{	
	.entry = (kprobe_opcode_t *) netlog_inet_stream_connect,
	.kp = 
	{
		.symbol_name = "inet_stream_connect",
		.fault_handler = handler_fault,
	},
};

static struct kretprobe connect_kretprobe = 
{
        .handler = post_connect,
        .maxactive = 0,
        .kp = 
        {
        	.symbol_name = "inet_stream_connect",
		.fault_handler = handler_fault,
        },
};

static struct kretprobe accept_kretprobe = 
{
	.handler = post_accept,
	.maxactive = 0,
        .kp = 
        {
		#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 32)
        	.symbol_name = "sys_accept",
        	#else
        	.symbol_name = "sys_accept4",
		#endif
		.fault_handler = handler_fault,
        },
};

#if PROBE_CONNECTION_CLOSE

static struct jprobe tcp_close_jprobe = 
{	
	.entry = (kprobe_opcode_t *) netlog_sys_close,
	.kp = 
	{
		.symbol_name = "sys_close",
		.fault_handler = handler_fault,
	}
};

#endif

#if PROBE_UDP

static struct jprobe bind_jprobe = 
{	
	.entry = (kprobe_opcode_t *) netlog_sys_bind,
	.kp = 
	{
		.symbol_name = "sys_bind",
		.fault_handler = handler_fault,
	},
};

#endif

void unplant_all(void)
{
  	unregister_jprobe(&connect_jprobe);
	printk(KERN_INFO MODULE_NAME "connect pre probe unplanted\n");
	unregister_kretprobe(&connect_kretprobe);
	printk(KERN_INFO MODULE_NAME "connect post probe unplanted\n");
	unregister_kretprobe(&accept_kretprobe);
	printk(KERN_INFO MODULE_NAME "accept probe unplanted\n");

	#if PROBE_CONNECTION_CLOSE

	unregister_jprobe(&tcp_close_jprobe);
	printk(KERN_INFO MODULE_NAME "inet_shutdown probe unplanted\n");

	#endif

	#if PROBE_UDP

  	unregister_jprobe(&bind_jprobe);
	printk(KERN_INFO MODULE_NAME "bind probe unplanted\n");

	#endif

	printk(KERN_INFO MODULE_NAME "Probes unplanted\n");
}

void netlog_exit(void)
{
	unplant_all();
	destroy_logger();
	printk(KERN_INFO MODULE_NAME "Logging facility destroyed\n");
}

/************************************/
/*             INIT MODULE          */
/************************************/

int __init plant_probes(void)
{
	int register_status;
	#if WHITELISTING
	int i;
	#endif	

	if(LOG_FAILED(init_logger(MODULE_NAME)))
	{
		printk(KERN_ERR MODULE_NAME "Failed to initialize logging facility\n");
		return LOG_FAILURE;
	}
	else
	{
		printk(KERN_INFO MODULE_NAME "Initialized logging facility\n");
	}

	register_status = register_jprobe(&connect_jprobe);

	if(register_status < 0)
	{
		printk(KERN_ERR MODULE_NAME "Failed to plant connect pre handler\n");
		netlog_exit();
		return CONNECT_PROBE_FAILED;
	}

	register_status = register_kretprobe(&connect_kretprobe);

	if(register_status < 0)
	{
		printk(KERN_ERR MODULE_NAME "Failed to plant connect post handler\n");
		netlog_exit();
		return CONNECT_PROBE_FAILED;
	}

	register_status = register_kretprobe(&accept_kretprobe);

	if(register_status < 0)
	{
		printk(KERN_ERR MODULE_NAME "Failed to plant accept post handler\n");
		netlog_exit();
		return ACCEPT_PROBE_FAILED;
	}

	#if PROBE_CONNECTION_CLOSE

	register_status = register_jprobe(&tcp_close_jprobe);

	if(register_status < 0)
	{
		printk(KERN_ERR MODULE_NAME "Failed to plant close pre handler\n");
		netlog_exit();
		return CLOSE_PROBE_FAILED;
	}

	#endif
	
	#if PROBE_UDP

	register_status = register_jprobe(&bind_jprobe);

	if(register_status < 0)
	{
		printk(KERN_ERR MODULE_NAME "Failed to plant bind pre handler\n");
		netlog_exit();
		return BIND_PROBE_FAILED;
	}

	#endif

	printk(KERN_INFO MODULE_NAME "All probes planted\n");

	#if WHITELISTING

	/*Deal with the whitelisting*/

	for(i = 0; i < NO_WHITELISTS; ++i)
	{
		if(WHITELIST_FAILED(whitelist(procs_to_whitelist[i])))
		{
			printk(KERN_ERR MODULE_NAME "Failed to whitelist %s\n", procs_to_whitelist[i]);
		}
		else
		{
			printk(KERN_INFO MODULE_NAME "Whitelisted %s\n", procs_to_whitelist[i]);
		}
	}

	#endif

	return 0;
}

/************************************/
/*             EXIT MODULE          */
/************************************/

void __exit unplant_probes(void)
{
	netlog_exit();
}

