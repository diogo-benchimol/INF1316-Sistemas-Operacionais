/*
 * KernelSim_T2.c
 *
 * Micro-kernel simulator (T2)
 * - Manages N_APPS application processes (A1..A5)
 * - Uses a remote SFSS (UDP) for file/directory operations
 * - Uses shared memory per-app for replies from SFSS
 *
 * Corrections and improvements:
 * - Fixed incorrect kill(...) that used index instead of PID.
 * - Bound UDP socket locally (INADDR_ANY:0) to ensure replies arrive.
 * - Full implementation included (app, interrupt controller, kernel).
 *
 * Usage:
 *   ./KernelSim_T2           (kernel)
 *   ./KernelSim_T2 inter     (interrupt controller)
 *   ./KernelSim_T2 app <id>  (application process, id = 1..5)
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/select.h>
#include <sys/types.h>
#include <fcntl.h>

#include "sfp_protocol.h"

/* ---------------- Configuration ---------------- */

#define N_APPS       5
#define MAX_BLOCKED  N_APPS
#define MAX_READY    N_APPS
#define QUANTUM_US   500000     /* 0.5 s quantum for apps/interrupt pacing */
#define MAX_PC       20         /* max instructions per app */
#define SYSCALL_PROB 10         /* 1 in SYSCALL_PROB chance per tick */

#define IRQ1_PROB    3  /* 1/3 chance for IRQ1 generation */
#define IRQ2_PROB    5  /* 1/5 chance for IRQ2 generation */

#define SFSS_HOST "127.0.0.1"
#define SFSS_PORT 8888

#define SHM_KEY_BASE 0x1316

/* ---------------- Types & Globals ---------------- */

enum ProcState { READY = 0, RUNNING = 1, BLOCKED = 2, TERMINATED = 3 };

typedef struct PCB {
    pid_t pid;                 /* OS PID of process */
    int   id;                  /* logical ID A1..AN (1..N_APPS) */
    int   state;               /* ProcState */
    int   pc;                  /* last program counter observed */
    SfpMessage pending_syscall;/* saved syscall for snapshot */
} PCB;

/* Global PCBs and scheduler structures */
static PCB pcbs[N_APPS];
static int running_idx = -1;

/* Queues to hold responses coming from SFSS (replies) */
static SfpMessage file_req_q[MAX_BLOCKED];
static int fq_h = 0, fq_t = 0, fq_sz = 0;

static SfpMessage dir_req_q[MAX_BLOCKED];
static int dq_h = 0, dq_t = 0, dq_sz = 0;

/* Ready queue (round-robin) */
static int rq[MAX_READY];
static int rq_h = 0, rq_t = 0, rq_sz = 0;

/* Pipes descriptors for intercontroller and apps (kernel reads) */
static int inter_r = -1, app_r = -1;
static pid_t inter_pid = -1;

/* Network and shared memory */
static int udp_sockfd = -1;
static struct sockaddr_in sfss_addr;
static int shm_ids[N_APPS];
static SfpMessage* shm_ptrs[N_APPS];

/* Flags for signals */
static volatile sig_atomic_t inter_pending = 0;
static volatile sig_atomic_t app_pending   = 0;
static volatile sig_atomic_t want_snapshot = 0;
static volatile sig_atomic_t want_resume   = 0;
static int paused = 0;

/* Local intercontroller pause flag (used inside inter process) */
static volatile sig_atomic_t ic_paused = 0;

/* ---------------- Utility helpers ---------------- */

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

/* write a newline-terminated literal to fd */
static ssize_t writeln(int fd, const char *s) {
    return write(fd, s, strlen(s));
}

static const char* state_str(int s) {
    return s == READY ? "READY" :
           s == RUNNING ? "RUNNING" :
           s == BLOCKED ? "BLOCKED" :
           s == TERMINATED ? "TERMINATED" : "?";
}

/* convert OS pid -> index in pcbs[] or -1 */
static int pid_to_index(pid_t pid) {
    for (int i = 0; i < N_APPS; ++i)
        if (pcbs[i].pid == pid) return i;
    return -1;
}

/* ---------------- Ready queue ops ---------------- */

static void rq_push_tail(int idx) {
    if (rq_sz >= MAX_READY) return;
    if (pcbs[idx].state == TERMINATED) return;
    rq[rq_t] = idx;
    rq_t = (rq_t + 1) % MAX_READY;
    rq_sz++;
}

static int rq_pop_head(void) {
    if (rq_sz == 0) return -1;
    int v = rq[rq_h];
    rq_h = (rq_h + 1) % MAX_READY;
    rq_sz--;
    return v;
}

/* ---------------- Scheduler ---------------- */

/* Choose next READY process and CONT it; stop current running process */
/* Escalonador principal (seleciona próximo processo READY) */
static void schedule_next(void){
    // Tenta encontrar um processo pronto
    int tries = rq_sz;
    while (tries-- > 0){
        int next = rq_pop_head();
        if (next < 0) break;

        // Se encontrou um processo pronto, roda ele
        if (pcbs[next].state == READY){
            // Interrompe o processo anterior se estava rodando
            if (running_idx >= 0 && pcbs[running_idx].state==RUNNING){
                kill(pcbs[running_idx].pid, SIGSTOP);
                pcbs[running_idx].state = READY;
                rq_push_tail(running_idx); // Bota o processo de volta no fim da fila
            }
            // Continua o novo processo selecionado
            kill(pcbs[next].pid, SIGCONT);
            pcbs[next].state = RUNNING;
            running_idx = next;
            fprintf(stderr,"[Kernel] Now running A%d (PID %d)\n", next+1, pcbs[next].pid);
            return;
        } else if (pcbs[next].state != TERMINATED) {
             // Se pegou um processo que não está READY (ex: BLOCKED),
             // bota de volta na fila.
             rq_push_tail(next);
        }
        // Se for TERMINATED, ele é descartado da fila
    }

    // Caso não haja processos prontos (fila vazia ou só com lixo)
    if (running_idx >= 0 && pcbs[running_idx].state==RUNNING){
        // Para o processo que estava rodando (se houver um)
        kill(pcbs[running_idx].pid, SIGSTOP);
        pcbs[running_idx].state = READY;
        rq_push_tail(running_idx); // Bota o processo de volta na fila
    }

    // Se a ready-queue realmente está vazia, MAS existem PCBs com state==READY,
    // algo deixou processos "não enfileirados". Reconstruímos a fila a partir dos estados.
    if (rq_sz == 0) {
        int found_ready = 0;
        for (int i = 0; i < N_APPS; ++i) {
            if (pcbs[i].state == READY) {
                rq_push_tail(i);
                found_ready = 1;
            }
        }
        if (found_ready) {
            // Temos agora itens na fila; tenta escalonar novamente.
            schedule_next();
            return;
        }

        // Caso realmente não haja ninguém READY, checamos se existem processos BLOCKED.
        running_idx = -1;
        int blocked = 0;
        for(int i=0; i<N_APPS; i++) {
            if (pcbs[i].state == BLOCKED) {
                blocked = 1;
                break;
            }
        }
        if (blocked == 0) {
            fprintf(stderr,"[Kernel] IDLE (no READY processes)\n");
        }
    } else {
        // A fila de prontos tem itens, mas nenhum está READY
        // (ex: todos foram bloqueados), então ficamos IDLE.
        running_idx = -1;
    }
}


/* ---------------- Signal handlers (kernel) ---------------- */

static void h_usr1(int s) { (void)s; inter_pending = 1; } /* IRQ from intercontroller */
static void h_usr2(int s) { (void)s; app_pending   = 1; } /* messages from apps (via pipe) */
static void h_int (int s) { (void)s; want_snapshot = 1; } /* SIGINT (Ctrl-C) -> snapshot */
static void h_cont(int s) { (void)s; want_resume   = 1; } /* SIGCONT -> resume */

/* ---------------- Snapshot printing ---------------- */

static void print_snapshot(void) {
    fprintf(stderr, "================ SNAPSHOT (paused) PID=%d =================\n", (int)getpid());
    for (int i = 0; i < N_APPS; ++i) {
        PCB *p = &pcbs[i];
        fprintf(stderr, "A%d (PID %d): PC=%d, state=%s", p->id, (int)p->pid, p->pc, state_str(p->state));
        if (p->state == BLOCKED) {
            fprintf(stderr, ", waiting SFP_MSG %d", p->pending_syscall.msg_type);
        }
        if (p->state == TERMINATED) fprintf(stderr, " (TERMINATED)");
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "READY Q: ");
    if (rq_sz == 0) fprintf(stderr, "(empty)\n");
    else {
        for (int k = 0, i = rq_h; k < rq_sz; ++k, i = (i + 1) % MAX_READY)
            fprintf(stderr, "A%d ", rq[i] + 1);
        fprintf(stderr, "\n");
    }
    if (running_idx >= 0) fprintf(stderr, "RUNNING: A%d\n", running_idx + 1);
    else fprintf(stderr, "RUNNING: (none)\n");
    fprintf(stderr, "File-Q: %d waiting / Dir-Q: %d waiting\n", fq_sz, dq_sz);
    fprintf(stderr, "=============================================================\n");
}

/* ---------------- Interrupt Controller process ---------------- */

/* Local handlers inside the intercontroller process */
static void ic_h_int(int s)  { (void)s; ic_paused = 1;  }
static void ic_h_cont(int s) { (void)s; ic_paused = 0;  }

static void run_interrupt_controller(void) {
    signal(SIGINT,  ic_h_int);
    signal(SIGCONT, ic_h_cont);

    srand((unsigned)(time(NULL) ^ getpid()));

    for (;;) {
        if (ic_paused) { usleep(100000); continue; }
        usleep(QUANTUM_US);
        writeln(STDOUT_FILENO, "IRQ0\n");
        kill(getppid(), SIGUSR1);

        /* probabilistic IRQ1 / IRQ2 */
        if (rand() % IRQ1_PROB == 0) {
            writeln(STDOUT_FILENO, "IRQ1\n");
            kill(getppid(), SIGUSR1);
        }
        if (rand() % IRQ2_PROB == 0) {
            writeln(STDOUT_FILENO, "IRQ2\n");
            kill(getppid(), SIGUSR1);
        }
    }
}

/* ---------------- Accumulator helpers for pipe reading ---------------- */

static int acc_append(char acc[], int acc_cap, int *acc_len, const char src[], int n) {
    int copied = 0;
    while (copied < n && *acc_len < acc_cap) {
        acc[*acc_len] = src[copied];
        (*acc_len)++;
        copied++;
    }
    return copied;
}

static int acc_find_nl(const char acc[], int len) {
    for (int i = 0; i < len; ++i) if (acc[i] == '\n') return i;
    return -1;
}

static void acc_copy_line(const char acc[], int linelen, char dest[], int dest_cap) {
    int k = linelen;
    if (k >= dest_cap) k = dest_cap - 1;
    for (int i = 0; i < k; ++i) dest[i] = acc[i];
    dest[k] = '\0';
}

static void acc_consume_line(char acc[], int *acc_len, int linelen) {
    int new_len = *acc_len - (linelen + 1);
    if (new_len < 0) new_len = 0;
    for (int i = 0; i < new_len; ++i)
        acc[i] = acc[linelen + 1 + i];
    *acc_len = new_len;
}

/* ---------------- Application process ---------------- */

static void run_app(int id) {
    /* ignore SIGINT inside app; parent handles snapshot */
    signal(SIGINT, SIG_IGN);

    /* start stopped — kernel will schedule (SIGCONT) */
    raise(SIGSTOP);

    /* random seed */
    srand((unsigned)(time(NULL) ^ getpid()));

    /* attach shmem for this app */
    key_t shm_key = SHM_KEY_BASE + id;
    int shm_id = shmget(shm_key, sizeof(SfpMessage), 0666);
    if (shm_id < 0) {
        fprintf(stderr, "[App A%d] shmget failed (key 0x%x)\n", id, (unsigned)shm_key);
        _exit(1);
    }
    SfpMessage *shm_ptr = (SfpMessage*) shmat(shm_id, NULL, 0);
    if (shm_ptr == (void*)-1) {
        fprintf(stderr, "[App A%d] shmat failed\n", id);
        _exit(1);
    }

    fprintf(stderr, "[App A%d] started, attached to shmem (shm_id=%d)\n", id, shm_id);

    int pc = 0;
    while (pc < MAX_PC) {
        usleep(QUANTUM_US);
        pc++;

        /* emit TICK message to kernel via stdout pipe */
        char tick[128];
        int tn = snprintf(tick, sizeof(tick), "TICK A%d %d %d\n", id, (int)getpid(), pc);
        write(STDOUT_FILENO, tick, tn);
        kill(getppid(), SIGUSR2);

        /* probabilistic syscall */
        if (rand() % SYSCALL_PROB == 0) {
            char msg[1024];
            int op_type = rand() % 5; /* 0=read,1=write,2=add,3=rem,4=list */

            switch (op_type) {
                case 0: { /* READ */
                    char path[128];
                    snprintf(path, sizeof(path), "/A%d/file.txt", (rand()%2==0)?id:0);
                    int offset = (rand() % 4) * 16;
                    snprintf(msg, sizeof(msg), "READ A%d %d %s %d\n", id, (int)getpid(), path, offset);
                    break;
                }
                case 1: { /* WRITE */
                    char path[128];
                    snprintf(path, sizeof(path), "/A%d/file.txt", (rand()%2==0)?id:0);
                    int offset = (rand() % 4) * 16;
                    snprintf(msg, sizeof(msg), "WRITE A%d %d %s %d HelloA%dPC%d\n",
                             id, (int)getpid(), path, offset, id, pc);
                    break;
                }
                case 2: { /* ADD (directory create) */
                    char path[128];
                    snprintf(path, sizeof(path), "/A%d", (rand()%2==0)?id:0);
                    snprintf(msg, sizeof(msg), "ADD A%d %d %s newDir_A%d_%d\n",
                             id, (int)getpid(), path, id, pc);
                    break;
                }
                case 3: { /* REM (directory remove) */
                    char path[128];
                    snprintf(path, sizeof(path), "/A%d", (rand()%2==0)?id:0);
                    snprintf(msg, sizeof(msg), "REM A%d %d %s newDir_A%d_%d\n",
                             id, (int)getpid(), path, id, pc>0?pc-1:0);
                    break;
                }
                case 4: { /* LISTDIR */
                    char path[128];
                    snprintf(path, sizeof(path), "/A%d", (rand()%2==0)?id:0);
                    snprintf(msg, sizeof(msg), "LISTDIR A%d %d %s\n", id, (int)getpid(), path);
                    break;
                }
                default:
                    msg[0] = '\0';
            }

            /* send syscall line to kernel */
            write(STDOUT_FILENO, msg, strlen(msg));
            kill(getppid(), SIGUSR2);

            /* stop and wait for kernel to unblock via SIGCONT */
            raise(SIGSTOP);

            /* upon wake-up, read shmem result and print outcome */
            fprintf(stderr, "[App A%d] Woke up — checking shmem reply\n", id);

            SfpMessage *r = shm_ptr;
            switch (r->msg_type) {
                case SFP_MSG_RD_REP:
                    if (r->offset >= 0) {
                        /* payload may not be null-terminated; print as binary-safe */
                        int len = SFP_PAYLOAD_SIZE;
                        fprintf(stderr, "[App A%d] READ OK @ offset=%d payload='", id, r->offset);
                        fwrite(r->payload, 1, len, stderr);
                        fprintf(stderr, "'\n");
                    } else {
                        fprintf(stderr, "[App A%d] READ ERROR code=%d\n", id, r->offset);
                    }
                    break;
                case SFP_MSG_WR_REP:
                    if (r->offset >= 0) fprintf(stderr, "[App A%d] WRITE OK @ offset=%d\n", id, r->offset);
                    else fprintf(stderr, "[App A%d] WRITE ERROR code=%d\n", id, r->offset);
                    break;
                case SFP_MSG_DC_REP:
                    if (r->path_len >= 0) fprintf(stderr, "[App A%d] DIR CREATE OK -> %s\n", id, r->path);
                    else fprintf(stderr, "[App A%d] DIR CREATE ERROR code=%d\n", id, r->path_len);
                    break;
                case SFP_MSG_DR_REP:
                    if (r->path_len >= 0) fprintf(stderr, "[App A%d] DIR REMOVE OK -> %s\n", id, r->path);
                    else fprintf(stderr, "[App A%d] DIR REMOVE ERROR code=%d\n", id, r->path_len);
                    break;
                case SFP_MSG_DL_REP:
                    if (r->nrnames >= 0) fprintf(stderr, "[App A%d] LISTDIR OK -> %d entries\n", id, r->nrnames);
                    else fprintf(stderr, "[App A%d] LISTDIR ERROR code=%d\n", id, r->nrnames);
                    break;
                default:
                    fprintf(stderr, "[App A%d] Unexpected SFP msg in shmem: %d\n", id, r->msg_type);
            }
        }

        usleep(QUANTUM_US);
    } /* end while */

    /* send DONE notification to kernel */
    char done[128];
    int dn = snprintf(done, sizeof(done), "DONE A%d %d %d\n", id, (int)getpid(), pc);
    write(STDOUT_FILENO, done, dn);
    kill(getppid(), SIGUSR2);

    /* detach and exit */
    shmdt(shm_ptr);
    _exit(0);
}

/* ---------------- Kernel: handle replies from SFSS (UDP recv) ---------------- */

static void handle_sfs_reply(void) {
    SfpMessage res_msg;
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    ssize_t n = recvfrom(udp_sockfd, &res_msg, sizeof(SfpMessage), 0,
                         (struct sockaddr*)&from_addr, &from_len);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return;
        perror("[Kernel] recvfrom error");
        return;
    }

    fprintf(stderr, "[Kernel] Received SFP msg %d from SFSS for owner %d\n",
            res_msg.msg_type, res_msg.owner);

    switch (res_msg.msg_type) {
        case SFP_MSG_RD_REP:
        case SFP_MSG_WR_REP:
            if (fq_sz < MAX_BLOCKED) {
                file_req_q[fq_t] = res_msg;
                fq_t = (fq_t + 1) % MAX_BLOCKED;
                fq_sz++;
            } else {
                fprintf(stderr, "[Kernel] File queue full — dropping reply\n");
            }
            break;

        case SFP_MSG_DC_REP:
        case SFP_MSG_DR_REP:
        case SFP_MSG_DL_REP:
            if (dq_sz < MAX_BLOCKED) {
                dir_req_q[dq_t] = res_msg;
                dq_t = (dq_t + 1) % MAX_BLOCKED;
                dq_sz++;
            } else {
                fprintf(stderr, "[Kernel] Dir queue full — dropping reply\n");
            }
            break;

        default:
            fprintf(stderr, "[Kernel] Unknown reply type from SFSS: %d\n", res_msg.msg_type);
    }
}

/* ---------------- Kernel: drain intercontroller pipe (IRQ lines) ---------------- */

static void drain_inter(void) {
    static char acc[1024];
    static int acc_len = 0;
    char buf[256];
    ssize_t n = read(inter_r, buf, sizeof(buf));
    if (n <= 0) { if (n < 0 && errno == EINTR) return; return; }
    acc_append(acc, (int)sizeof(acc), &acc_len, buf, (int)n);
    if (acc_len >= (int)sizeof(acc)) acc_len = 0;

    for (;;) {
        int pos = acc_find_nl(acc, acc_len);
        if (pos < 0) break;
        char line[128];
        acc_copy_line(acc, pos, line, (int)sizeof(line));
        acc_consume_line(acc, &acc_len, pos);

        if (strcmp(line, "IRQ0") == 0) {
            /* Round-robin quantum expiration */
            if (running_idx >= 0 && pcbs[running_idx].state == RUNNING) {
                int cur = running_idx;
                pcbs[cur].state = READY;
                rq_push_tail(cur);
                kill(pcbs[cur].pid, SIGSTOP);
                running_idx = -1;
            }
            schedule_next();

        } else if (strcmp(line, "IRQ1") == 0) {
            /* File I/O done: pop file_req_q and unblock owner */
            if (fq_sz > 0) {
                SfpMessage res_msg = file_req_q[fq_h];
                fq_h = (fq_h + 1) % MAX_BLOCKED;
                fq_sz--;

                int owner = res_msg.owner;
                int idx = owner - 1;
                if (idx >= 0 && idx < N_APPS && pcbs[idx].state == BLOCKED) {
                    /* copy into shared mem for that process */
                    memcpy(shm_ptrs[idx], &res_msg, sizeof(SfpMessage));
                    pcbs[idx].state = READY;
                    rq_push_tail(idx);
                    fprintf(stderr, "[Kernel] IRQ1 -> unblocked A%d (PID %d) enqueued\n",
                            idx + 1, (int)pcbs[idx].pid);
                    if (running_idx == -1) schedule_next();
                } else {
                    fprintf(stderr, "[Kernel] IRQ1 -> WARN owner A%d not found or not blocked\n", owner);
                }
            }
        } else if (strcmp(line, "IRQ2") == 0) {
            /* Dir I/O done: pop dir_req_q and unblock owner */
            if (dq_sz > 0) {
                SfpMessage res_msg = dir_req_q[dq_h];
                dq_h = (dq_h + 1) % MAX_BLOCKED;
                dq_sz--;

                int owner = res_msg.owner;
                int idx = owner - 1;
                if (idx >= 0 && idx < N_APPS && pcbs[idx].state == BLOCKED) {
                    memcpy(shm_ptrs[idx], &res_msg, sizeof(SfpMessage));
                    pcbs[idx].state = READY;
                    rq_push_tail(idx);
                    fprintf(stderr, "[Kernel] IRQ2 -> unblocked A%d (PID %d) enqueued\n",
                            idx + 1, (int)pcbs[idx].pid);
                    if (running_idx == -1) schedule_next();
                } else {
                    fprintf(stderr, "[Kernel] IRQ2 -> WARN owner A%d not found or not blocked\n", owner);
                }
            }
        } else {
            fprintf(stderr, "[Kernel] Unknown IRQ line: '%s'\n", line);
        }
    }
}

/* ---------------- Kernel: drain apps pipe (app messages and syscalls) ---------------- */

static void drain_apps(void) {
    static char acc[4096];
    static int acc_len = 0;
    char buf[512];
    ssize_t n = read(app_r, buf, sizeof(buf));
    if (n <= 0) { if (n < 0 && errno == EINTR) return; return; }
    acc_append(acc, (int)sizeof(acc), &acc_len, buf, (int)n);
    if (acc_len >= (int)sizeof(acc)) acc_len = 0;

    for (;;) {
        int pos = acc_find_nl(acc, acc_len);
        if (pos < 0) break;
        char line[512];
        acc_copy_line(acc, pos, line, (int)sizeof(line));
        acc_consume_line(acc, &acc_len, pos);

        int aid = 0, pid = 0;
        if (strncmp(line, "TICK", 4) == 0) {
            int pc = 0;
            if (sscanf(line, "TICK A%d %d %d", &aid, &pid, &pc) == 3) {
                int idx = pid_to_index((pid_t)pid);
                if (idx >= 0 && pcbs[idx].state != TERMINATED) pcbs[idx].pc = pc;
            }
        } else if (strncmp(line, "DONE", 4) == 0) {
            int pc = 0;
            if (sscanf(line, "DONE A%d %d %d", &aid, &pid, &pc) == 3) {
                int idx = pid_to_index((pid_t)pid);
                if (idx >= 0 && pcbs[idx].state != TERMINATED) {
                    pcbs[idx].pc = pc;
                    pcbs[idx].state = TERMINATED;
                    fprintf(stderr, "[Kernel] (app msg) A%d (PID %d) finished.\n", aid, pid);
                    if (idx == running_idx) {
                        running_idx = -1;
                        schedule_next();
                    }
                }
            }
        } else {
            /* parse syscalls: READ, WRITE, ADD, REM, LISTDIR */
            SfpMessage req_msg;
            memset(&req_msg, 0, sizeof(req_msg));
            int idx = -1;
            char path_buf[SFP_MAX_PATH_LEN];
            char name_buf[SFP_MAX_PATH_LEN];
            char payload_buf[SFP_PAYLOAD_SIZE + 32];
            int offset = 0;

            if (sscanf(line, "READ A%d %d %s %d", &aid, &pid, path_buf, &offset) == 4) {
                idx = pid_to_index((pid_t)pid);
                req_msg.msg_type = SFP_MSG_RD_REQ;
                req_msg.owner = aid;
                strncpy(req_msg.path, path_buf, SFP_MAX_PATH_LEN);
                req_msg.path[SFP_MAX_PATH_LEN - 1] = '\0';
                req_msg.path_len = strlen(req_msg.path);
                req_msg.offset = offset;

            } else if (sscanf(line, "WRITE A%d %d %s %d %s", &aid, &pid, path_buf, &offset, payload_buf) == 5) {
                idx = pid_to_index((pid_t)pid);
                req_msg.msg_type = SFP_MSG_WR_REQ;
                req_msg.owner = aid;
                strncpy(req_msg.path, path_buf, SFP_MAX_PATH_LEN);
                req_msg.path[SFP_MAX_PATH_LEN - 1] = '\0';
                req_msg.path_len = strlen(req_msg.path);
                req_msg.offset = offset;
                /* copy payload (truncate/pad to SFP_PAYLOAD_SIZE) */
                memset(req_msg.payload, 0, SFP_PAYLOAD_SIZE);
                strncpy(req_msg.payload, payload_buf, SFP_PAYLOAD_SIZE);

            } else if (sscanf(line, "ADD A%d %d %s %s", &aid, &pid, path_buf, name_buf) == 4) {
                idx = pid_to_index((pid_t)pid);
                req_msg.msg_type = SFP_MSG_DC_REQ;
                req_msg.owner = aid;
                strncpy(req_msg.path, path_buf, SFP_MAX_PATH_LEN);
                req_msg.path[SFP_MAX_PATH_LEN - 1] = '\0';
                req_msg.path_len = strlen(req_msg.path);
                strncpy(req_msg.name, name_buf, SFP_MAX_PATH_LEN);
                req_msg.name[SFP_MAX_PATH_LEN - 1] = '\0';
                req_msg.name_len = strlen(req_msg.name);

            } else if (sscanf(line, "REM A%d %d %s %s", &aid, &pid, path_buf, name_buf) == 4) {
                idx = pid_to_index((pid_t)pid);
                req_msg.msg_type = SFP_MSG_DR_REQ;
                req_msg.owner = aid;
                strncpy(req_msg.path, path_buf, SFP_MAX_PATH_LEN);
                req_msg.path[SFP_MAX_PATH_LEN - 1] = '\0';
                req_msg.path_len = strlen(req_msg.path);
                strncpy(req_msg.name, name_buf, SFP_MAX_PATH_LEN);
                req_msg.name[SFP_MAX_PATH_LEN - 1] = '\0';
                req_msg.name_len = strlen(req_msg.name);

            } else if (sscanf(line, "LISTDIR A%d %d %s", &aid, &pid, path_buf) == 3) {
                idx = pid_to_index((pid_t)pid);
                req_msg.msg_type = SFP_MSG_DL_REQ;
                req_msg.owner = aid;
                strncpy(req_msg.path, path_buf, SFP_MAX_PATH_LEN);
                req_msg.path[SFP_MAX_PATH_LEN - 1] = '\0';
                req_msg.path_len = strlen(req_msg.path);
            } else {
                /* unknown line */
                fprintf(stderr, "[Kernel] Unknown app line: '%s'\n", line);
            }

            if (idx != -1) {
                if (idx >= 0 && pcbs[idx].state != TERMINATED) {
                    /* block the process and save pending syscall for snapshot */
                    pcbs[idx].state = BLOCKED;
                    pcbs[idx].pending_syscall = req_msg;
                    fprintf(stderr, "[Kernel] SYSCALL A%d (PID %d): MSG %d -> BLOCKED\n",
                            idx + 1, pid, req_msg.msg_type);

                    /* send request to SFSS via UDP */
                    ssize_t sent = sendto(udp_sockfd, &req_msg, sizeof(SfpMessage), 0,
                                          (struct sockaddr*)&sfss_addr, sizeof(sfss_addr));
                    if (sent < 0) {
                        perror("[Kernel] sendto failed");
                    }

                    /* remove from CPU if it was running */
                    if (idx == running_idx) {
                        running_idx = -1;
                        schedule_next();
                    } else if (running_idx == -1) {
                        schedule_next();
                    }
                }
            }
        }
    }
}

/* ---------------- Kernel main loop & startup ---------------- */

static void run_kernel(void) {
    fprintf(stderr, "[Kernel] PID=%d\n", (int)getpid());

    /* create UDP socket */
    if ((udp_sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) die("socket udp");

    memset(&sfss_addr, 0, sizeof(sfss_addr));
    sfss_addr.sin_family = AF_INET;
    sfss_addr.sin_port = htons(SFSS_PORT);
    if (inet_pton(AF_INET, SFSS_HOST, &sfss_addr.sin_addr) <= 0) die("inet_pton SFSS_HOST");

    /* Bind locally to an ephemeral port to ensure we can receive replies predictably */
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0; /* ephemeral port */
    if (bind(udp_sockfd, (struct sockaddr*)&local, sizeof(local)) < 0) {
        /* non-fatal: warn and continue */
        perror("[Kernel] warning: bind udp_sockfd failed");
    }

    /* create pipes for reading child's stdout (intercontroller and apps) */
    int inter_p[2], app_p[2];
    if (pipe(inter_p) == -1 || pipe(app_p) == -1) die("pipe");

    /* fork intercontroller process */
    inter_pid = fork();
    if (inter_pid == -1) die("fork inter");
    if (inter_pid == 0) {
        /* child: intercontroller */
        close(inter_p[0]);
        dup2(inter_p[1], STDOUT_FILENO);
        close(inter_p[1]);
        /* close app pipe ends in this process */
        close(app_p[0]); close(app_p[1]);
        execlp("./KernelSim_T2", "KernelSim_T2", "inter", NULL);
        die("exec inter");
    }

    /* create shared memory and fork apps */
    for (int i = 0; i < N_APPS; ++i) {
        /* create shared mem for app (keys use i+1 so app ids 1..N_APPS match) */
        key_t shm_key = SHM_KEY_BASE + (i + 1);
        int shm_id = shmget(shm_key, sizeof(SfpMessage), IPC_CREAT | 0666);
        if (shm_id < 0) die("shmget");
        SfpMessage* shm_ptr = (SfpMessage*) shmat(shm_id, NULL, 0);
        if (shm_ptr == (void*)-1) die("shmat");

        fprintf(stderr, "[Kernel] Created shmem for A%d (key=0x%x, id=%d)\n",
                i + 1, (unsigned)shm_key, shm_id);

        shm_ids[i] = shm_id;
        shm_ptrs[i] = shm_ptr;

        pid_t p = fork();
        if (p == -1) die("fork app");
        if (p == 0) {
            /* child: app process */
            close(app_p[0]);
            dup2(app_p[1], STDOUT_FILENO);
            close(app_p[1]);
            /* close intercontroller pipe ends in this process */
            close(inter_p[0]); close(inter_p[1]);
            char idstr[8];
            snprintf(idstr, sizeof(idstr), "%d", i + 1);
            execlp("./KernelSim_T2", "KernelSim_T2", "app", idstr, NULL);
            die("exec app");
        }

        /* parent: kernel continues */
        pcbs[i].pid = p;
        pcbs[i].id = i + 1;
        pcbs[i].state = READY;
        pcbs[i].pc = 0;
    }

    /* close write ends in kernel, keep read ends */
    close(inter_p[1]);
    close(app_p[1]);
    inter_r = inter_p[0];
    app_r = app_p[0];

    /* install signal handlers for kernel */
    signal(SIGUSR1, h_usr1);
    signal(SIGUSR2, h_usr2);
    signal(SIGINT,  h_int);
    signal(SIGCONT, h_cont);

    /* initialize ready queue with all processes */
    rq_h = rq_t = rq_sz = 0;
    for (int i = 0; i < N_APPS; ++i) rq_push_tail(i);

    running_idx = -1;
    schedule_next(); /* start first process */

    fprintf(stderr, "[Kernel] Started. Running A1 (PID %d)\n", (int)pcbs[0].pid);

    /* main loop: pselect to wait either for UDP data or for signals */
    for (;;) {
        fd_set read_fds;
        sigset_t empty_mask;
        sigemptyset(&empty_mask);

        FD_ZERO(&read_fds);
        FD_SET(udp_sockfd, &read_fds); /* we listen for UDP replies */

        inter_pending = 0;
        app_pending = 0;

        int r = pselect(udp_sockfd + 1, &read_fds, NULL, NULL, NULL, &empty_mask);
        if (r < 0) {
            if (errno == EINTR) {
                /* expected; signals will be handled below */
            } else {
                perror("[Kernel] pselect error");
                continue;
            }
        }

        /* if UDP socket had data */
        if (r > 0 && FD_ISSET(udp_sockfd, &read_fds)) {
            handle_sfs_reply();
        }

        /* snapshot (Ctrl-C) */
        if (want_snapshot) {
            want_snapshot = 0;
            paused = 1;
            if (inter_pid > 0) kill(inter_pid, SIGINT);
            /* BUG FIX: use PID (not index) when stopping running process */
            if (running_idx >= 0 && pcbs[running_idx].state == RUNNING) {
                kill(pcbs[running_idx].pid, SIGSTOP); /* correct PID usage */
            }
            print_snapshot();
        }

        /* resume after snapshot (SIGCONT) */
        if (want_resume) {
            want_resume = 0;
            paused = 0;
            if (inter_pid > 0) kill(inter_pid, SIGCONT);
            if (running_idx >= 0 && pcbs[running_idx].state == RUNNING) {
                kill(pcbs[running_idx].pid, SIGCONT);
            }
            fprintf(stderr, "[Kernel] Resumed.\n");
        }

        /* process pending events if not paused */
        if (!paused) {
            if (inter_pending) drain_inter();
            if (app_pending)   drain_apps();
        }

        /* reap terminated children (non-blocking) */
        int status;
        pid_t reap_pid;
        while ((reap_pid = waitpid(-1, &status, WNOHANG)) > 0) {
            int idx = pid_to_index(reap_pid);
            if (idx >= 0 && pcbs[idx].state != TERMINATED) {
                pcbs[idx].state = TERMINATED;
                fprintf(stderr, "[Kernel] (reap) A%d (PID %d) TERMINATED\n", idx + 1, (int)reap_pid);
                if (idx == running_idx) {
                    running_idx = -1;
                    schedule_next();
                }
            }
        }

        /* check if any app is still alive */
        int alive = 0;
        for (int i = 0; i < N_APPS; ++i) if (pcbs[i].state != TERMINATED) { alive = 1; break; }

        if (!alive) {
            /* clean up */
            if (inter_pid > 0) {
                kill(inter_pid, SIGTERM);
                waitpid(inter_pid, NULL, 0);
            }
            if (inter_r >= 0) close(inter_r);
            if (app_r >= 0) close(app_r);
            if (udp_sockfd >= 0) close(udp_sockfd);

            for (int i = 0; i < N_APPS; ++i) {
                shmdt(shm_ptrs[i]);
                shmctl(shm_ids[i], IPC_RMID, NULL);
            }

            fprintf(stderr, "[Kernel] All apps terminated. Exiting.\n");
            break;
        }
    } /* main for */
}

/* ---------------- Main entrypoint ---------------- */

int main(int argc, char *argv[]) {
    if (argc == 1) {
        run_kernel();
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "inter") == 0) {
        run_interrupt_controller();
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "app") == 0) {
        int id = atoi(argv[2]);
        if (id < 1 || id > N_APPS) id = 1;
        run_app(id);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "app") == 0 && argc == 3) {
        int id = atoi(argv[2]);
        run_app(id);
        return 0;
    }

    fprintf(stderr,
            "Usage:\n"
            "  ./KernelSim_T2             (kernel)\n"
            "  ./KernelSim_T2 inter       (interrupt controller)\n"
            "  ./KernelSim_T2 app <id>    (app, id 1..5)\n");
    return 1;
}
