/*
 * Copyright (c) 2017, Xiaoye Meng
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/inet.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <net/sock.h>
#include <net/tcp.h>
#include "shfe.h"

/* FIXME */
struct client {
	struct socket		*sock;
	struct task_struct	*task;
	struct timer_list	timer;
	struct quote		quote;
	u32			heartbeat;
	u32			dataready;
	u32			connected;
	u32			disconnected;
	u32			inpos;
	unsigned char		inbuf[64 * 1024 * 1024];
	unsigned char		debuf[8192];
};

/* FIXME */
static char *front_ip;
static int front_port;
static char *brokerid, *userid, *passwd, *contract;
module_param(front_ip, charp, 0000);
module_param(front_port, int, 0000);
module_param(brokerid, charp, 0000);
module_param(userid,   charp, 0000);
module_param(passwd,   charp, 0000);
module_param(contract, charp, 0000);
static struct client sh;

/* FIXME */
static int quotes_send(struct socket *sock, unsigned char *buf, int len) {
	struct msghdr msg;
	struct kvec iov;

	msg.msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL;
	iov.iov_base  = buf;
	iov.iov_len   = len;
	return kernel_sendmsg(sock, &msg, &iov, 1, len);
}

/* FIXME */
static int quotes_recv(struct socket *sock, unsigned char *buf, int len) {
	struct msghdr msg;
	struct kvec iov;

	msg.msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL;
	iov.iov_base  = buf;
	iov.iov_len   = len;
	return kernel_recvmsg(sock, &msg, &iov, 1, len, msg.msg_flags);
}

/* FIXME */
static void quotes_sock_data_ready(struct sock *sk, int unused) {
	struct client *c = (struct client *)sk->sk_user_data;

	printk(KERN_INFO "[%s] state = %d", __func__, sk->sk_state);
	if (sk->sk_state != TCP_CLOSE && sk->sk_state != TCP_CLOSE_WAIT)
		atomic_inc((atomic_t *)&c->dataready);
}

/* FIXME */
static void quotes_sock_write_space(struct sock *sk) {
	printk(KERN_INFO "[%s] state = %d", __func__, sk->sk_state);
}

/* FIXME */
static void quotes_sock_state_change(struct sock *sk) {
	struct client *c = (struct client *)sk->sk_user_data;

	printk(KERN_INFO "[%s] state = %d", __func__, sk->sk_state);
	switch (sk->sk_state) {
	case TCP_CLOSE:
		printk(KERN_INFO "[%s] TCP_CLOSE", __func__);
	case TCP_CLOSE_WAIT:
		printk(KERN_INFO "[%s] TCP_CLOSE_WAIT", __func__);
		atomic_set((atomic_t *)&c->disconnected, 1);
		break;
	case TCP_ESTABLISHED:
		printk(KERN_INFO "[%s] TCP_ESTABLISHED", __func__);
		atomic_set((atomic_t *)&c->connected, 1);
		break;
	default:
		break;
	}
}

/* FIXME */
static void set_sock_callbacks(struct socket *sock, struct client *c) {
	struct sock *sk = sock->sk;

	sk->sk_user_data    = (void *)c;
	sk->sk_data_ready   = quotes_sock_data_ready;
	sk->sk_write_space  = quotes_sock_write_space;
	sk->sk_state_change = quotes_sock_state_change;
}

/* FIXME */
static void send_hbtimeout(struct client *c) {
	struct hbtimeout to;

	to.ftd_type        = 0x00;
	to.ftd_extd_length = 0x06;
	to.ftd_cont_length = 0x0000;
	to.tag_type        = 0x07;
	to.tag_length      = 0x04;
	to.timeout         = 0x27000000;
	quotes_send(c->sock, (unsigned char *)&to, sizeof to);
}

/* FIXME */
static void send_heartbeat(struct client *c) {
	struct heartbeat hb;

	hb.ftd_type        = 0x00;
	hb.ftd_extd_length = 0x02;
	hb.ftd_cont_length = 0x0000;
	hb.tag_type        = 0x05;
	hb.tag_length      = 0x00;
	quotes_send(c->sock, (unsigned char *)&hb, sizeof hb);
}

static void login(struct client *c) {
	struct login lo;
	struct timeval time;
	struct tm tm;
	struct net *net = sock_net(c->sock->sk);
	struct net_device *dev;

	memset(&lo, '\0', sizeof lo);
	lo.header.ftd_type         = 0x02;
	lo.header.ftd_extd_length  = 0x00;
	lo.header.ftd_cont_length  = 0xea00;
	lo.header.version          = 0x01;
	lo.header.ftdc_type        = 0x00;
	lo.header.unenc_length     = 0x0b;
	lo.header.chain            = 0x4c;
	lo.header.seq_series       = 0x0000;
	lo.header.seq_number       = 0x00300000;
	lo.header.prv_number       = 0x00000000;
	lo.header.fld_count        = 0x0300;
	lo.header.ftdc_cont_length = 0xd400;
	lo.header.rid              = 0x01000000;
	lo.type                    = 0x0210;
	lo.length                  = 0xbc00;
	do_gettimeofday(&time);
	time_to_tm(time.tv_sec, 0, &tm);
	snprintf(lo.td_day, sizeof lo.td_day, "%04ld%02d%02d",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
	memcpy(lo.brokerid, brokerid, sizeof lo.brokerid - 1);
	memcpy(lo.userid,   userid,   sizeof lo.userid   - 1);
	memcpy(lo.passwd,   passwd,   sizeof lo.passwd   - 1);
	/* FIXME */
	lo.ipi[0]                  = 'T';
	lo.ipi[1]                  = 'H';
	lo.ipi[2]                  = 'O';
	lo.ipi[3]                  = 'S';
	lo.ipi[4]                  = 'T';
	lo.ipi[5]                  = ' ';
	lo.ipi[6]                  = 'U';
	lo.ipi[7]                  = 's';
	lo.ipi[8]                  = 'e';
	lo.ipi[9]                  = 'r';
	lo.pi[0]                   = 'F';
	lo.pi[1]                   = 'T';
	lo.pi[2]                   = 'D';
	lo.pi[3]                   = 'C';
	lo.pi[4]                   = ' ';
	lo.pi[5]                   = '0';
	/* FIXME */
	for_each_netdev(net, dev)
		if (strcmp(dev->name, "lo")) {
			snprintf(lo.mac, sizeof lo.mac, "%02X:%02X:%02X:%02X:%02X:%02X",
				dev->dev_addr[0],
				dev->dev_addr[1],
				dev->dev_addr[2],
				dev->dev_addr[3],
				dev->dev_addr[4],
				dev->dev_addr[5]);
			break;
		}
	lo.type1                   = 0x0110;
	lo.length1                 = 0x0600;
	lo.seq_series1             = 0x0100;
	lo.seq_number1             = 0x00000000;
	lo.type4                   = 0x0110;
	lo.length4                 = 0x0600;
	lo.seq_series4             = 0x0400;
	lo.seq_number4             = 0x00000000;
	quotes_send(c->sock, (unsigned char *)&lo, sizeof lo);
}

/* FIXME */
static void subscribe(struct client *c) {
	struct subscribe sb;

	memset(&sb, '\0', sizeof sb);
	sb.header.ftd_type         = 0x02;
	sb.header.ftd_extd_length  = 0x00;
	sb.header.ftd_cont_length  = 0x3900;
	sb.header.version          = 0x01;
	sb.header.ftdc_type        = 0x00;
	sb.header.unenc_length     = 0x0b;
	sb.header.chain            = 0x4c;
	sb.header.seq_series       = 0x0000;
	sb.header.seq_number       = 0x01440000;
	sb.header.prv_number       = 0x00000000;
	sb.header.fld_count        = 0x0100;
	sb.header.ftdc_cont_length = 0x2300;
	sb.header.rid              = 0x00000000;
	sb.type                    = 0x4124;
	sb.length                  = 0x1f00;
	memcpy(sb.instid, contract, sizeof sb.instid - 1);
	quotes_send(c->sock, (unsigned char *)&sb, sizeof sb);
}

/* FIXME */
static void print_buf(unsigned char *buf, int len) {
	int i;

	for (i = 0; i + 7 < len; i += 8)
		printk(KERN_INFO "[%s] <%d> %02x %02x %02x %02x %02x %02x %02x %02x",
			__func__, len,
			buf[i],
			buf[i + 1],
			buf[i + 2],
			buf[i + 3],
			buf[i + 4],
			buf[i + 5],
			buf[i + 6],
			buf[i + 7]);
	for (; i < len; ++i)
		printk(KERN_INFO "[%s] <%d> %02x ", __func__, len, buf[i]);
}

/* FIXME */
static void handle_mdpacket(struct client *c, unsigned short *type) {
	switch (*type) {
	case 0x3124:
		{
			struct mdbase *mdbase = (struct mdbase *)((unsigned char *)type + 4);

			memcpy(c->quote.td_day, mdbase->td_day, sizeof c->quote.td_day);
			c->quote.presettle  = mdbase->presettle;
			c->quote.preclose   = mdbase->preclose;
			c->quote.preopenint = mdbase->preopenint;
			c->quote.predelta   = mdbase->predelta;
		}
		break;
	case 0x3224:
		{
			struct mdstatic *mdstatic = (struct mdstatic *)((unsigned char *)type + 4);

			c->quote.open       = mdstatic->open;
			c->quote.high       = mdstatic->high;
			c->quote.low        = mdstatic->low;
			c->quote.close      = mdstatic->close;
			c->quote.upperlimit = mdstatic->upperlimit;
			c->quote.lowerlimit = mdstatic->lowerlimit;
			c->quote.settle     = mdstatic->settle;
			c->quote.delta      = mdstatic->delta;
		}
		break;
	case 0x3324:
		{
			struct mdlast *mdlast = (struct mdlast *)((unsigned char *)type + 4);

			c->quote.last       = mdlast->last;
			c->quote.volume     = mdlast->volume;
			c->quote.turnover   = mdlast->turnover;
			c->quote.openint    = mdlast->openint;
		}
		break;
	case 0x3424:
		{
			struct mdbest *mdbest = (struct mdbest *)((unsigned char *)type + 4);

			c->quote.bid1       = mdbest->bid1;
			c->quote.bvol1      = mdbest->bvol1;
			c->quote.ask1       = mdbest->ask1;
			c->quote.avol1      = mdbest->avol1;
		}
		break;
	case 0x3524:
		{
			struct mdbid23 *mdbid23 = (struct mdbid23 *)((unsigned char *)type + 4);

			c->quote.bid2       = mdbid23->bid2;
			c->quote.bvol2      = mdbid23->bvol2;
			c->quote.bid3       = mdbid23->bid3;
			c->quote.bvol3      = mdbid23->bvol3;
		}
		break;
	case 0x3624:
		{
			struct mdask23 *mdask23 = (struct mdask23 *)((unsigned char *)type + 4);

			c->quote.ask2       = mdask23->ask2;
			c->quote.avol2      = mdask23->avol2;
			c->quote.ask3       = mdask23->ask3;
			c->quote.avol3      = mdask23->avol3;
		}
		break;
	case 0x3724:
		{
			struct mdbid45 *mdbid45 = (struct mdbid45 *)((unsigned char *)type + 4);

			c->quote.bid4       = mdbid45->bid4;
			c->quote.bvol4      = mdbid45->bvol4;
			c->quote.bid5       = mdbid45->bid5;
			c->quote.bvol5      = mdbid45->bvol5;
		}
		break;
	case 0x3824:
		{
			struct mdask45 *mdask45 = (struct mdask45 *)((unsigned char *)type + 4);

			c->quote.ask4       = mdask45->ask4;
			c->quote.avol4      = mdask45->avol4;
			c->quote.ask5       = mdask45->ask5;
			c->quote.avol5      = mdask45->avol5;
		}
		break;
	case 0x3924:
		{
			struct mdtime *mdtime = (struct mdtime *)((unsigned char *)type + 4);

			memcpy(c->quote.instid, mdtime->instid, sizeof c->quote.instid);
			memcpy(c->quote.time,   mdtime->time,   sizeof c->quote.time);
			c->quote.msec       = mdtime->msec;
			memcpy(c->quote.at_day, mdtime->at_day, sizeof c->quote.at_day);
		}
		break;
	case 0x8124:
		{
			double *average = (double *)((unsigned char *)type + 4);

			c->quote.average    = *average;
		}
		break;
	}
}

static void process_inbuf(struct client *c) {
	unsigned char *start = c->inbuf;

	while (c->inpos >= 4) {
		unsigned char  ftd_type     = start[0];
		unsigned char  ftd_extd_len = start[1];
		unsigned short ftd_cont_len = ntohs(*((unsigned short *)(start + 2)));

		/* packet is incomplete */
		if (c->inpos < ftd_cont_len + 4)
			break;
		if (ftd_type == 0x02 && ftd_extd_len == 0x00 && ftd_cont_len > 0) {
			int i, j;
			struct shfeheader *sh = (struct shfeheader *)c->debuf;

			/* FIXME */
			c->debuf[0] = start[0];
			c->debuf[1] = start[1];
			c->debuf[2] = start[2];
			c->debuf[3] = start[3];
			c->debuf[4] = start[4];
			c->debuf[5] = start[5];
			c->debuf[6] = start[6];
			c->debuf[7] = start[7];
			for (i = 8, j = 8; i < ftd_cont_len + 4; ++i)
				if (start[i] == 0xe0)
					c->debuf[j++] = start[i++ + 1];
				else if (start[i] >= 0xe1 && start[i] <= 0xef) {
					int k, n = start[i] - 0xe0;

					for (k = 0; k < n; ++k)
						c->debuf[j++] = 0x00;
				} else
					c->debuf[j++] = start[i];
			print_buf(c->debuf, j);
			switch (sh->seq_number) {
			case 0x01300000:
				{
					struct info *info = (struct info *)(sh->buf + 4);

					if (info->errid == 0)
						subscribe(c);
				}
				break;
			case 0x02440000:
				{
					struct info *info = (struct info *)(sh->buf + 4);
					char *contract = (char *)(sh->buf + 4 + sizeof *info + 4);

					if (info->errid != 0)
						printk(KERN_INFO "[%s] subscribe '%s' NOT OK",
							__func__, contract);
				}
				break;
			case 0x01f10000:
				{
					unsigned short *type   = (unsigned short *)sh->buf;
					unsigned short *length = (unsigned short *)(sh->buf + 2);
					struct quote *quote    = (struct quote *)(sh->buf + 4);

					if (*type == 0x1200 && *length == 0x6201) {
						unsigned char *p;

						c->quote = *quote;
						p = (unsigned char *)&c->quote;
						print_buf(p, sizeof c->quote);
					}
				}
				break;
			case 0x03f10000:
				{
					unsigned short count   = ntohs(sh->fld_count);
					unsigned short *type   = (unsigned short *)sh->buf;
					unsigned short *length = (unsigned short *)(sh->buf + 2);
					unsigned char *p;

					for (i = 0; i < count; ++i) {
						handle_mdpacket(c, type);
						type = (unsigned short *)((unsigned char *)type +
							4 + ntohs(*length));
						length = (unsigned short *)((unsigned char *)type +
							2);
					}
					p = (unsigned char *)&c->quote;
					print_buf(p, sizeof c->quote);
				}
				break;
			}
		} else if (ftd_type == 0x00 && ftd_extd_len == 0x02) {
			printk(KERN_INFO "[%s] receiving heartbeat", __func__);
			ftd_cont_len = ftd_extd_len;
		} else
			printk(KERN_ERR "[%s] unknown packet type = 0x%02x, length = %d",
				__func__, ftd_type, ftd_cont_len);
		start    += ftd_cont_len + 4;
		c->inpos -= ftd_cont_len + 4;
	}
	if (start != c->inbuf && c->inpos != 0)
		memmove(c->inbuf, start, c->inpos);
}

/* FIXME */
static void timer_func(unsigned long data) {
	struct client *c = (struct client *)data;

	atomic_set((atomic_t *)&c->heartbeat, 1);
	init_timer(&c->timer);
	c->timer.expires  = jiffies + 15 * HZ;
	c->timer.function = timer_func;
	c->timer.data     = (unsigned long)c;
	add_timer(&c->timer);
}

static int quotes_connect(struct client *c) {
	struct sockaddr_in s;

	if (sock_create_kern(PF_INET, SOCK_STREAM, IPPROTO_TCP, &c->sock) < 0) {
		printk(KERN_ERR "[%s] error creating quotes socket", __func__);
		return -EIO;
	}
	/* FIXME */
	c->sock->sk->sk_allocation = GFP_NOFS;
	set_sock_callbacks(c->sock, c);
	s.sin_family      = AF_INET;
	s.sin_addr.s_addr = in_aton(front_ip);
	s.sin_port        = htons(front_port);
	return c->sock->ops->connect(c->sock, (struct sockaddr *)&s, sizeof s, O_NONBLOCK);
}

static int recv_thread(void *data) {
	struct client *c = (struct client *)data;

	while (!kthread_should_stop()) {
		if (atomic_read((atomic_t *)&c->heartbeat)) {
			printk(KERN_INFO "[%s] sending heartbeat", __func__);
			send_heartbeat(c);
			atomic_set((atomic_t *)&c->heartbeat, 0);
		}
		if (atomic_read((atomic_t *)&c->dataready)) {
			int len;

			len = quotes_recv(c->sock, c->inbuf + c->inpos, sizeof c->inbuf - c->inpos);
			/* FIXME */
			if (len) {
				c->inpos += len;
				process_inbuf(c);
			}
			atomic_dec((atomic_t *)&c->dataready);
		}
		if (atomic_read((atomic_t *)&c->connected)) {
			init_timer(&c->timer);
			c->timer.expires  = jiffies + 15 * HZ;
			c->timer.function = timer_func;
			c->timer.data     = (unsigned long)c;
			add_timer(&c->timer);
			printk(KERN_INFO "[%s] sending heartbeat timeout", __func__);
			send_hbtimeout(c);
			printk(KERN_INFO "[%s] logging in", __func__);
			login(c);
			atomic_set((atomic_t *)&c->connected, 0);
		}
		if (atomic_read((atomic_t *)&c->disconnected)) {
			int ret, one = 1;

			if (timer_pending(&c->timer))
				del_timer(&c->timer);
			atomic_set((atomic_t *)&c->heartbeat, 0);

loop:
			sock_release(c->sock);
			/* FIXME */
			schedule_timeout_uninterruptible(15 * HZ);
			/* FIXME */
			if ((ret = quotes_connect(c)) == -EINPROGRESS) {
			} else if (ret < 0) {
				printk(KERN_ERR "[%s] error reconnecting", __func__);
				goto loop;
			}
			/* FIXME */
			kernel_setsockopt(c->sock, SOL_TCP, TCP_NODELAY, (char *)&one, sizeof one);
			atomic_set((atomic_t *)&c->disconnected, 0);
		}
	}
	return 0;
}

static int __init quotes_init(void) {
	int ret, one = 1;

	if (front_ip == NULL || front_port == 0 || brokerid == NULL ||
		userid == NULL || passwd == NULL || contract == NULL) {
		printk(KERN_ERR "[%s] front_ip, front_port, brokerid, "
			"userid, passwd or contract can't be NULL", __func__);
		return -EINVAL;
	}
	/* FIXME */
	if ((ret = quotes_connect(&sh)) == -EINPROGRESS) {
	} else if (ret < 0) {
		printk(KERN_ERR "[%s] error connecting", __func__);
		goto end;
	}
	/* FIXME */
	kernel_setsockopt(sh.sock, SOL_TCP, TCP_NODELAY, (char *)&one, sizeof one);
	sh.task = kthread_create(recv_thread, &sh, "quotes_sh");
	if (IS_ERR(sh.task)) {
		printk(KERN_ERR "[%s] error creating quotes thread", __func__);
		goto end;
	}
	kthread_bind(sh.task, 1);
	wake_up_process(sh.task);
	return 0;

end:
	sock_release(sh.sock);
	return -EIO;
}

/* FIXME */
static void __exit quotes_exit(void) {
	if (timer_pending(&sh.timer))
		del_timer(&sh.timer);
	kthread_stop(sh.task);
	sock_release(sh.sock);
}

module_init(quotes_init);
module_exit(quotes_exit);
MODULE_AUTHOR("Xiaoye Meng");
MODULE_LICENSE("Dual BSD/GPL");

