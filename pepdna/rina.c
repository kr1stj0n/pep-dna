/*
 *  rina/pepdna/rina.c: PEP-DNA RINA support
 *
 *  Copyright (C) 2020  Kristjon Ciko <kristjoc@ifi.uio.no>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#include "rina.h"
#include "core.h"
#include "connection.h"
#include "netlink.h"
#include "tcp_utils.h"

#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,11,0)
#include <linux/signal.h>
#else
#include <linux/sched/signal.h>
#endif


/*
 * Check if RINA flow is not destroyed
 * ------------------------------------------------------------------------- */
bool flow_is_ok(struct ipcp_flow *flow)
{
        if (!flow) {
                //pep_debug("Flow is NULL");
                return false; /* Flow is NULL */
        }

        if (flow && flow->state < 0) { /* flow->state not valid */
                //pep_debug("Flow state invalid");
                return false;
        }

        if (flow && !flow->wqs) {
                //pep_debug("Flow wqs are NULL");
                return false; /* Flow waitqueues are NULL */
        }

        if (flow->state == PORT_STATE_ALLOCATED || flow->state == PORT_STATE_DISABLED) {
                //pep_debug("Flow ALLOCATED | DISABLED");
                return true;
        }
        return false;
}

/*
 * Check if flow is readable
 * ------------------------------------------------------------------------- */
bool queue_is_ready(struct ipcp_flow *flow)
{
        if (!flow_is_ok(flow))
                return true;

        if (flow && flow->state != PORT_STATE_PENDING
                        &&!rfifo_is_empty(flow->sdu_ready)) {
                if (!rfifo_is_empty(flow->sdu_ready))
                        return true;
        }

        return false;
}

/*
 * Wait for RINA flow to become readable
 * ------------------------------------------------------------------------- */
long pepdna_wait_for_sdu(struct ipcp_flow *flow)
{
        DEFINE_WAIT(wq_entry);
        signed long timeo = (signed long)usecs_to_jiffies(FLOW_POLL_TIMEOUT);

        for (;;) {
                if (flow_is_ok(flow))
                        prepare_to_wait(&flow->wqs->read_wqueue, &wq_entry,
                                        TASK_INTERRUPTIBLE);
                else {
                        timeo = -ESHUTDOWN;
                        return timeo;
                }
                if (timeo && rfifo_is_empty(flow->sdu_ready)) {
                        timeo = schedule_timeout(timeo);
                }
                if (queue_is_ready(flow))
                        break;
                if (!timeo)
                        break;
                if (signal_pending(current)) {
                        timeo = -ERESTARTSYS;
                        break;
                }
        }

        if (flow_is_ok(flow))
                finish_wait(&flow->wqs->read_wqueue, &wq_entry);
        else {
                timeo = -ESHUTDOWN;
                __set_current_state(TASK_RUNNING);
        }

        return timeo;
}

/*
 * Send DUs over a RINA flow
 * ------------------------------------------------------------------------- */
int pepdna_flow_write(struct ipcp_flow *flow, int pid, unsigned char *buf,
                      size_t len)
{
    struct ipcp_instance *ipcp = NULL;
    struct du *du              = NULL;
    size_t left                = len;
    size_t max_du_size         = 0;
    size_t copylen             = 0;
    size_t sent                = 0;
    int rc                     = 0;

    if (!flow) {
        pep_err("No flow bound to port_id %d", pid);
        return -EBADF;
    }

    if (flow->state < 0) {
        pep_err("Flow with port_id %d is already deallocated", pid);
        return -ESHUTDOWN;
    }

    ipcp = flow->ipc_process;

    max_du_size = ipcp->ops->max_sdu_size(ipcp->data);

    while (left) {
        copylen = min(left, max_du_size);
        du = du_create(copylen);
        if (!du) {
            rc = -ENOMEM;
            goto out;
        }

        memcpy(du_buffer(du), buf + sent, copylen);

        if (ipcp->ops->du_write(ipcp->data, pid, du, false)) {
            pep_err("Couldn't write SDU to port_id %d", pid);
            rc = -EIO;
            goto out;
        }

        left -= copylen;
        sent += copylen;
    }
out:
    if (sent == 0)
        return rc;
    else
        return sent;
}


void pepdna_flow_alloc(struct work_struct *work)
{
        struct pepdna_con *con = container_of(work, struct pepdna_con,
                                              tcfa_work);
        int rc = 0;

        /* Asking fallocator.client to initiate (1) a RINA flow allocation */
        rc = pepdna_nl_sendmsg(con->tuple.saddr, con->tuple.source,
                               con->tuple.daddr, con->tuple.dest,
                               con->hash_conn_id, atomic_read(&con->port_id), 1);
        if (rc < 0) {
                pep_err("Couldn't notify fallocator to allocate a flow");
                goto out;
        }

        return;
out:
        pepdna_con_put(con);
}

/*
 * Check if flow has already a valid port-id and a !NULL flow
 * ------------------------------------------------------------------------- */
bool flow_is_ready(struct pepdna_con *con)
{
    struct ipcp_flow *flow = kfa_flow_find_by_pid(kipcm_kfa(default_kipcm),
                                                  atomic_read(&con->port_id));
    if (flow) {
        pep_debug("Flow with port_id %d is now ready", atomic_read(&con->port_id));
        con->flow = flow;
    } else
        pep_debug("Flow with port_id %d is not ready yet", atomic_read(&con->port_id));

    return flow && atomic_read(&con->port_id);
}

/*
 * Forward data from RINA flow to TCP socket
 * ------------------------------------------------------------------------- */
int pepdna_con_r2i_fwd(struct pepdna_con *con)
{
    struct kfa *kfa      = kipcm_kfa(default_kipcm);
    struct socket *lsock = con->lsock;
    struct du *du        = NULL;
    int port_id          = atomic_read(&con->port_id);
    bool blocking        = false; /* Don't block while reading from the flow */
    signed long timeo    = 0;
    int rc = 0, rs       = 0;

    IRQ_BARRIER;

    while (rconnected(con)) {
        timeo = pepdna_wait_for_sdu(con->flow);
        if (timeo > 0)
            break;
        if (timeo == -ERESTARTSYS || timeo == -ESHUTDOWN || timeo == -EINTR) {
                rc = -1;
                goto out;
        }
    }

    rs = kfa_flow_du_read(kfa, port_id, &du, MAX_SDU_SIZE, blocking);
    if (rs <= 0) {
        pep_debug("kfa_flow_du_read %d", rs);
        return rs;
    }

    if (!is_du_ok(du))
        return -EIO;

    rc = pepdna_sock_write(lsock, du_buffer(du), rs);
    if (rc <= 0) {
        pep_debug("pepdna_sock_write %d", rc);
    }

    du_destroy(du);
out:
    return rc;
}

/*
 * Forward data from TCP socket to RINA flow
 * ------------------------------------------------------------------------- */
int pepdna_con_i2r_fwd(struct pepdna_con *con)
{
    struct socket *lsock   = con->lsock;
    struct ipcp_flow *flow = con->flow;
    unsigned char *buffer  = NULL;
    int port_id            = atomic_read(&con->port_id);
    int rc                 = 0, rs = 0;

    struct msghdr msg = {
        .msg_flags = MSG_DONTWAIT,
    };
    struct kvec vec;

    /* allocate buffer memory */
    buffer = kzalloc(MAX_BUF_SIZE, GFP_KERNEL);
    if (!buffer) {
        pep_err("Failed to alloc buffer");
        return -ENOMEM;
    }
    vec.iov_base = buffer;
    vec.iov_len  = MAX_BUF_SIZE;
    iov_iter_kvec(&msg.msg_iter, READ | ITER_KVEC, &vec, 1, vec.iov_len);
    rc = sock_recvmsg(lsock, &msg, MSG_DONTWAIT);
    if (rc > 0) {
        rs = pepdna_flow_write(flow, port_id, buffer, rc);
        if (rs <= 0) {
            pep_err("Couldn't write to flow %d", port_id);
            kfree(buffer);
            return rs;
        }
    } else {
        if (rc == -EAGAIN || rc == -EWOULDBLOCK)
            pep_debug("sock_recvmsg() says %d", rc);
    }

    kfree(buffer);
    return rc;
}