#define _USE_POSIX
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MIN_PROCS 3
#define MAX_PROCS 34
#define FICHERO_CONTROL "crisis.dat"

/* LPE IV y VIII: variables static en lugar de globales y maximo 34 hijos */
static pid_t hijos[MAX_PROCS];
static int num_hijos = 0;
static int finalizar = 0;
static int hay_sigchld = 0;
static int fd_fichero = -1;

/* Manejadora para SIGINT (cierre con CTRL-C) y SIGTERM (quiebra en cascada) */
static void manejadora_fin(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        finalizar = 1;
    }
}

/* Manejadora para SIGCHLD (evitar procesos zombies al morir hijos) */
static void manejadora_sigchld(int sig)
{
    if (sig == SIGCHLD) {
        hay_sigchld = 1;
    }
}

/* Funcion auxiliar para sacar texto por stdout con write (sin buffer printf) */
static int escribir_pantalla(const char *mensaje)
{
    size_t len;

    len = strlen(mensaje);
    while (write(1, mensaje, len) == -1) {
        if (errno != EINTR) {
            perror("write");
            return -1;
        }
    }
    return 0;
}

/* Imprime V(<PID>) o M(<PID>) usando escribir_pantalla */
static void imprimir_estado_pid(char letra)
{
    char buf[32];

    sprintf(buf, "%c(%d)\n", letra, (int)getpid());
    escribir_pantalla(buf);
}

/* Bloquea el fichero compartido desde el inicio mediante lockf */
static int bloquear_fichero(int fd)
{
    if (lseek(fd, 0, SEEK_SET) == -1) {
        perror("lseek");
        return -1;
    }
    while (lockf(fd, F_LOCK, 0) == -1) {
        if (errno != EINTR) {
            perror("lockf F_LOCK");
            return -1;
        }
    }
    return 0;
}

/* Desbloquea el fichero compartido */
static int desbloquear_fichero(int fd)
{
    if (lseek(fd, 0, SEEK_SET) == -1) {
        perror("lseek");
        return -1;
    }
    if (lockf(fd, F_ULOCK, 0) == -1) {
        perror("lockf F_ULOCK");
        return -1;
    }
    return 0;
}

/* Lee el numero de procesos vivos del fichero compartido */
static int leer_fichero(int fd, int *vivos)
{
    if (lseek(fd, 0, SEEK_SET) == -1) {
        perror("lseek");
        return -1;
    }
    if (read(fd, vivos, sizeof(int)) != (ssize_t)sizeof(int)) {
        perror("read");
        return -1;
    }
    return 0;
}

/* Escribe el numero de procesos vivos en el fichero compartido */
static int escribir_fichero(int fd, int vivos)
{
    if (lseek(fd, 0, SEEK_SET) == -1) {
        perror("lseek");
        return -1;
    }
    if (write(fd, &vivos, sizeof(int)) != (ssize_t)sizeof(int)) {
        perror("write");
        return -1;
    }
    return 0;
}

/* Quita un PID de la lista de hijos vivos del proceso */
static void quitar_hijo(pid_t pid)
{
    int i;
    int j;

    for (i = 0; i < num_hijos; ++i) {
        if (hijos[i] == pid) {
            for (j = i; j < num_hijos - 1; ++j) {
                hijos[j] = hijos[j + 1];
            }
            num_hijos = num_hijos - 1;
            break;
        }
    }
}

/* Decrementa el contador de vivos en el fichero cuando muere un hijo */
static void restar_proceso_vivo(void)
{
    int vivos;

    if (bloquear_fichero(fd_fichero) == 0) {
        if (leer_fichero(fd_fichero, &vivos) == 0 && vivos > 0) {
            vivos = vivos - 1;
            escribir_fichero(fd_fichero, vivos);
        }
        desbloquear_fichero(fd_fichero);
    }
}

int main(int argc, char *argv[])
{
    int n_max_procs;
    int velocidad; /* 0 = normal, 1 = veloz */
    int es_matriz;
    int vivos;
    int puede_crear;
    int estado;
    int i;
    long val;
    char *fin_ptr;
    pid_t pid_hijo;
    pid_t pid_muerto;
    struct sigaction sa;
    sigset_t bloq;
    sigset_t ant;
    struct timeval tv;

    /* 1. Validacion de argumentos: crisis n_max_procs [velocidad] */
    if (argc < 2 || argc > 3 || argv[1][0] == '\0') {
        fprintf(stderr, "Uso: %s n_max_procs [velocidad]\n", argv[0]);
        return 1;
    }

    errno = 0;
    fin_ptr = NULL;
    val = strtol(argv[1], &fin_ptr, 10);
    if (errno != 0 || *fin_ptr != '\0' ||
        val < MIN_PROCS || val > MAX_PROCS) {
        fprintf(stderr, "Error: n_max_procs debe estar entre 3 y 34.\n");
        return 1;
    }
    n_max_procs = (int)val;

    velocidad = 0;
    if (argc == 3) {
        if (strcmp(argv[2], "normal") == 0) {
            velocidad = 0;
        } else if (strcmp(argv[2], "veloz") == 0) {
            velocidad = 1;
        } else {
            fprintf(stderr, "Error: velocidad debe ser normal o veloz.\n");
            return 1;
        }
    }

    /* 2. Registro de senales con sigaction */
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = manejadora_fin;
    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    sa.sa_handler = manejadora_sigchld;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction SIGCHLD");
        return 1;
    }

    /* 3. Creacion e inicializacion del fichero compartido por la matriz */
    fd_fichero = open(FICHERO_CONTROL, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd_fichero == -1) {
        perror("open");
        return 1;
    }

    if (bloquear_fichero(fd_fichero) == -1) {
        close(fd_fichero);
        unlink(FICHERO_CONTROL);
        return 1;
    }
    if (escribir_fichero(fd_fichero, 1) == -1) {
        desbloquear_fichero(fd_fichero);
        close(fd_fichero);
        unlink(FICHERO_CONTROL);
        return 1;
    }
    desbloquear_fichero(fd_fichero);

    /* 4. Semilla aleatoria y nacimiento de la empresa matriz */
    gettimeofday(&tv, NULL);
    srand((unsigned int)(tv.tv_sec + tv.tv_usec + getpid()));
    es_matriz = 1;
    imprimir_estado_pid('V');

    /* 5. Bucle principal de la matriz y las filiales */
    while (!finalizar) {
        /* Si murio algun hijo, recogerlo sin bloquear (evita zombies) */
        if (hay_sigchld) {
            sigemptyset(&bloq);
            sigaddset(&bloq, SIGINT);
            sigaddset(&bloq, SIGTERM);
            sigaddset(&bloq, SIGCHLD);
            sigprocmask(SIG_BLOCK, &bloq, &ant);

            hay_sigchld = 0;
            while (num_hijos > 0) {
                pid_muerto = waitpid(-1, &estado, WNOHANG);
                if (pid_muerto > 0) {
                    quitar_hijo(pid_muerto);
                    restar_proceso_vivo();
                } else if (pid_muerto == -1 && errno == EINTR) {
                    continue;
                } else {
                    break;
                }
            }

            sigprocmask(SIG_SETMASK, &ant, NULL);
        }

        if (finalizar) {
            break;
        }

        /* Pausa de 1 a 4 segundos en modo normal; en veloz no hace nada */
        if (velocidad == 0) {
            sleep((unsigned int)(1 + (rand() % 4)));
        }

        if (finalizar) {
            break;
        }

        /* La matriz siempre intenta crear filial; la filial 50% o muere */
        if (es_matriz) {
            puede_crear = 1;
        } else {
            if ((rand() % 2) == 0) {
                puede_crear = 1;
            } else {
                raise(SIGTERM);
                break;
            }
        }

        if (puede_crear) {
            sigemptyset(&bloq);
            sigaddset(&bloq, SIGINT);
            sigaddset(&bloq, SIGTERM);
            sigaddset(&bloq, SIGCHLD);
            sigprocmask(SIG_BLOCK, &bloq, &ant);

            if (!finalizar && bloquear_fichero(fd_fichero) == 0) {
                if (leer_fichero(fd_fichero, &vivos) == 0 &&
                    vivos < n_max_procs) {
                    vivos = vivos + 1;
                    if (escribir_fichero(fd_fichero, vivos) == 0) {
                        desbloquear_fichero(fd_fichero);

                        pid_hijo = fork();
                        if (pid_hijo == -1) {
                            perror("fork");
                            restar_proceso_vivo();
                        } else if (pid_hijo == 0) {
                            /* Hijo: reinicia estado local y reabre O_RDWR */
                            es_matriz = 0;
                            num_hijos = 0;
                            finalizar = 0;
                            hay_sigchld = 0;

                            close(fd_fichero);
                            fd_fichero = open(FICHERO_CONTROL, O_RDWR);
                            if (fd_fichero == -1) {
                                perror("open hijo");
                                sigprocmask(SIG_SETMASK, &ant, NULL);
                                return 1;
                            }

                            gettimeofday(&tv, NULL);
                            srand((unsigned int)(tv.tv_sec + tv.tv_usec +
                                                 getpid()));
                            imprimir_estado_pid('V');
                        } else {
                            /* Creador: apunta el PID de su nuevo hijo */
                            if (num_hijos < MAX_PROCS) {
                                hijos[num_hijos] = pid_hijo;
                                num_hijos = num_hijos + 1;
                            }
                        }

                        sigprocmask(SIG_SETMASK, &ant, NULL);
                        continue;
                    }
                }
                desbloquear_fichero(fd_fichero);
            }

            sigprocmask(SIG_SETMASK, &ant, NULL);
        }
    }

    /* 6. Quiebra en cascada y espera bloqueante a todos los descendientes */
    sigemptyset(&bloq);
    sigaddset(&bloq, SIGINT);
    sigaddset(&bloq, SIGTERM);
    sigaddset(&bloq, SIGCHLD);
    sigprocmask(SIG_BLOCK, &bloq, &ant);

    for (i = 0; i < num_hijos; ++i) {
        kill(hijos[i], SIGTERM);
    }

    while (num_hijos > 0) {
        pid_muerto = waitpid(-1, &estado, 0);
        if (pid_muerto > 0) {
            quitar_hijo(pid_muerto);
            restar_proceso_vivo();
        } else if (pid_muerto == -1 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }

    sigprocmask(SIG_SETMASK, &ant, NULL);

    /* 7. Impresion de muerte y cierre del fichero compartido */
    imprimir_estado_pid('M');

    if (fd_fichero != -1) {
        close(fd_fichero);
    }

    if (es_matriz) {
        unlink(FICHERO_CONTROL);
        escribir_pantalla("Programa acabado correctamente\n");
    }

    return 0;
}