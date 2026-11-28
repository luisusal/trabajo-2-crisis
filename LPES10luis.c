#include <stdio.h>      // generales de c
#include <stdlib.h>     // generales de c
#include <string.h>     // para cadenas de texto
#include <unistd.h>     // llamadas al sistema basicas
#include <errno.h>      // define errno y codigos de error
#include <time.h>       // para time() en la semilla de srand
#include <fcntl.h>      // open y flags O_RDWR, O_CREAT, O_TRUNC
#include <sys/stat.h>   // permisos de fichero en open
#include <sys/types.h>  // pid_t, ssize_t
#include <sys/wait.h>   // waitpid y WNOHANG
#include <signal.h>     // sigaction, SIGINT, SIGTERM, SIGCHLD, kill

#define FICHERO_CONTROL "crisis.dat"

static int finalizar = 0;          // se pone a 1 en el padre al recibir SIGINT
static int morir_sigint = 0;       // se pone a 1 en el hijo al recibir SIGINT
static int morir_sigterm = 0;      // se pone a 1 en el hijo al recibir SIGTERM
static int hay_hijos_muertos = 0;  // se pone a 1 al recibir SIGCHLD

static void manejadora_sigint_padre(int sig)
{
    if (sig == SIGINT) {
        finalizar = 1;
    }
}

static void manejadora_sigint_hijo(int sig)
{
    if (sig == SIGINT) {
        morir_sigint = 1;
    }
}

static void manejadora_sigterm_hijo(int sig)
{
    if (sig == SIGTERM) {
        morir_sigterm = 1;
    }
}

static void manejadora_sigchld(int sig)
{
    if (sig == SIGCHLD) {
        hay_hijos_muertos = 1;
    }
}

int main(int argc, char *argv[])
{
    int n_max_procs;
    const char *velocidad = "normal"; // velocidad normal si no lo especifica

    // 1. Comprobar numero de argumentos:
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Uso: %s n_max_procs [velocidad]\n", argv[0]);
        return 1;
    }

    // 1. Comprobar y validar n_max_procs
    if (argv[1][0] == '\0') {
        fprintf(stderr, "Error: n_max_procs no puede estar vacio\n");
        return 1;
    }

    char *fin_ptr = NULL;
    errno = 0;
    long valor = strtol(argv[1], &fin_ptr, 10);

    if (errno != 0 || *fin_ptr != '\0' || valor < 3 || valor > 34) {
        fprintf(stderr, "Error: n_max_procs debe ser un entero entre 3 y 34.\n");
        return 1;
    }
    n_max_procs = (int)valor;

    // Validar el segundo arg opcional
    if (argc == 3) {
        if (strcmp(argv[2], "normal") == 0 || strcmp(argv[2], "veloz") == 0) {
            velocidad = argv[2];
        } else {
            fprintf(stderr, "Error: velocidad debe ser 'normal' o 'veloz'.\n");
            return 1;
        }
    }

    /* --- Depuracion (comentar cuando ya este comprobado) ---
    printf("[DEPURACION] argc = %d\n", argc);
    printf("[DEPURACION] argv[1] = '%s' -> n_max_procs = %d\n", argv[1], n_max_procs);
    printf("[DEPURACION] velocidad = '%s'\n", velocidad);
    //---------------------------------------------------------*/

    // Imprimir el numero maximo de procesos
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "Numero maximo de procesos: %d\n", n_max_procs);
    if (len > 0) {
        if (write(1, buf, (size_t)len) == -1) {
            perror("Error en write");
            return 1;
        }
    }

    // PASO 5:
    // Registrar SIGINT con sigaction para que el padre imprima al pulsar CTRL-C
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = manejadora_sigint_padre;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Error en sigaction SIGINT");
        return 1;
    }

    // PASO 9:
    // Registrar SIGCHLD con sigaction para recoger hijos muertos sin bloquearse
    struct sigaction sa_chld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = 0;
    sa_chld.sa_handler = manejadora_sigchld;
    if (sigaction(SIGCHLD, &sa_chld, NULL) == -1) {
        perror("Error en sigaction SIGCHLD");
        return 1;
    }

    // PASO 4:
    // Crear e inicializar el fichero compartido con 1 proceso vivo (el padre)
    int fd = open(FICHERO_CONTROL, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        perror("Error en open");
        return 1;
    }

    if (lseek(fd, 0, SEEK_SET) == -1) {
        perror("Error en lseek");
        close(fd);
        unlink(FICHERO_CONTROL);
        return 1;
    }
    while (lockf(fd, F_LOCK, 0) == -1) {
        if (errno != EINTR) {
            perror("Error en lockf F_LOCK");
            close(fd);
            unlink(FICHERO_CONTROL);
            return 1;
        }
    }

    int vivos = 1; // el proceso padre principal es el 1ero
    if (lseek(fd, 0, SEEK_SET) == -1) {
        perror("Error en lseek");
        lockf(fd, F_ULOCK, 0);
        close(fd);
        unlink(FICHERO_CONTROL);
        return 1;
    }
    if (write(fd, &vivos, sizeof(int)) != (ssize_t)sizeof(int)) {
        perror("Error en write en fichero");
        lockf(fd, F_ULOCK, 0);
        close(fd);
        unlink(FICHERO_CONTROL);
        return 1;
    }

    if (lseek(fd, 0, SEEK_SET) == -1) {
        perror("Error en lseek");
        close(fd);
        unlink(FICHERO_CONTROL);
        return 1;
    }
    if (lockf(fd, F_ULOCK, 0) == -1) {
        perror("Error en lockf F_ULOCK");
        close(fd);
        unlink(FICHERO_CONTROL);
        return 1;
    }

    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    while (!finalizar) {
        unsigned int segs = (unsigned int)(1 + (rand() % 4));
        sleep(segs);

        if (finalizar) {
            break;
        }

        // PASO 9:
        // Si ha llegado SIGCHLD, recoger hijos terminados sin bloquearse (WNOHANG)
        if (hay_hijos_muertos) {
            hay_hijos_muertos = 0;
            int estado;
            pid_t p_muerto;
            for (;;) {
                p_muerto = waitpid(-1, &estado, WNOHANG);
                if (p_muerto > 0) {
                    continue;
                }
                if (p_muerto == -1 && errno == EINTR) {
                    continue;
                }
                break;
            }
        }

        if (lseek(fd, 0, SEEK_SET) == -1) {
            perror("Error en lseek");
            continue;
        }
        while (lockf(fd, F_LOCK, 0) == -1) {
            if (errno != EINTR) {
                perror("Error en lockf F_LOCK");
                break;
            }
        }

        int hay_hueco = 0;
        if (!finalizar && lseek(fd, 0, SEEK_SET) != -1 &&
            read(fd, &vivos, sizeof(int)) == (ssize_t)sizeof(int)) {
            if (vivos < n_max_procs) {
                vivos++;
                if (lseek(fd, 0, SEEK_SET) != -1 &&
                    write(fd, &vivos, sizeof(int)) == (ssize_t)sizeof(int)) {
                    hay_hueco = 1;
                }
            }
        }

        if (lseek(fd, 0, SEEK_SET) != -1) {
            lockf(fd, F_ULOCK, 0);
        }

        if (hay_hueco && !finalizar) {
            pid_t pid = fork();

            if (pid == -1) {
                perror("Error en el fork");
                if (lseek(fd, 0, SEEK_SET) != -1) {
                    while (lockf(fd, F_LOCK, 0) == -1 && errno == EINTR) {
                    }
                    if (lseek(fd, 0, SEEK_SET) != -1 &&
                        read(fd, &vivos, sizeof(int)) == (ssize_t)sizeof(int) &&
                        vivos > 1) {
                        vivos--;
                        if (lseek(fd, 0, SEEK_SET) != -1) {
                            write(fd, &vivos, sizeof(int));
                        }
                    }
                    if (lseek(fd, 0, SEEK_SET) != -1) {
                        lockf(fd, F_ULOCK, 0);
                    }
                }
            } else if (pid == 0) {
                // El hijo no hereda la bandera de hijos muertos de su padre
                hay_hijos_muertos = 0;

                // PASO 6:
                // 1. Registrar SIGINT y SIGTERM en el hijo
                morir_sigint = 0;
                morir_sigterm = 0;

                struct sigaction sa_hijo;
                sigemptyset(&sa_hijo.sa_mask);
                sa_hijo.sa_flags = 0;

                sa_hijo.sa_handler = manejadora_sigint_hijo;
                if (sigaction(SIGINT, &sa_hijo, NULL) == -1) {
                    perror("Error en sigaction SIGINT del hijo");
                    close(fd);
                    return 1;
                }

                sa_hijo.sa_handler = manejadora_sigterm_hijo;
                if (sigaction(SIGTERM, &sa_hijo, NULL) == -1) {
                    perror("Error en sigaction SIGTERM del hijo");
                    close(fd);
                    return 1;
                }

                // Cerrar descriptor heredado y abrir con O_RDWR (sin O_TRUNC)
                close(fd);
                fd = open(FICHERO_CONTROL, O_RDWR);
                if (fd == -1) {
                    perror("Error en open del hijo");
                    return 1;
                }

                // Semilla propia del hijo combinando tiempo y su PID
                srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

                // 2. Imprimir V(<PID>) justo al nacer el hijo
                char msg_nac[32];
                int len_nac = snprintf(msg_nac, sizeof(msg_nac), "V(%d)\n", (int)getpid());
                if (len_nac > 0) {
                    if (write(1, msg_nac, (size_t)len_nac) == -1) {
                        perror("Error en write de nacimiento");
                        close(fd);
                        return 1;
                    }
                }

                // PASO 7 y PASO 10:
                // Bucle del hijo: duerme al azar (1..4 s), recoge hijos muertos,
                // echa numero al azar para ver si muere (autoenviando SIGTERM)
                // o, si no muere y hay hueco, crea su propio hijo como el padre
                while (!morir_sigint && !morir_sigterm) {
                    unsigned int segs_hijo = (unsigned int)(1 + (rand() % 4));
                    sleep(segs_hijo);

                    if (morir_sigint || morir_sigterm) {
                        break;
                    }

                    if (hay_hijos_muertos) {
                        hay_hijos_muertos = 0;
                        int estado;
                        pid_t p_muerto;
                        for (;;) {
                            p_muerto = waitpid(-1, &estado, WNOHANG);
                            if (p_muerto > 0) {
                                continue;
                            }
                            if (p_muerto == -1 && errno == EINTR) {
                                continue;
                            }
                            break;
                        }
                    }

                    if (morir_sigint || morir_sigterm) {
                        break;
                    }

                    int azar_muerte = rand() % 2; // 0 -> muere, 1 -> no muere
                    if (azar_muerte == 0) {
                        if (kill(getpid(), SIGTERM) == -1) {
                            perror("Error en kill SIGTERM");
                            close(fd);
                            return 1;
                        }
                        break;
                    }

                    // PASO 10: si no muere, comprobar si hay hueco y crear un hijo
                    if (lseek(fd, 0, SEEK_SET) == -1) {
                        perror("Error en lseek del hijo");
                        continue;
                    }
                    while (lockf(fd, F_LOCK, 0) == -1) {
                        if (errno != EINTR) {
                            perror("Error en lockf F_LOCK del hijo");
                            break;
                        }
                    }

                    int hueco_hijo = 0;
                    if (!morir_sigint && !morir_sigterm && lseek(fd, 0, SEEK_SET) != -1 &&
                        read(fd, &vivos, sizeof(int)) == (ssize_t)sizeof(int)) {
                        if (vivos < n_max_procs) {
                            vivos++;
                            if (lseek(fd, 0, SEEK_SET) != -1 &&
                                write(fd, &vivos, sizeof(int)) == (ssize_t)sizeof(int)) {
                                hueco_hijo = 1;
                            }
                        }
                    }

                    if (lseek(fd, 0, SEEK_SET) != -1) {
                        lockf(fd, F_ULOCK, 0);
                    }

                    if (hueco_hijo && !morir_sigint && !morir_sigterm) {
                        pid_t nieto = fork();
                        if (nieto == -1) {
                            perror("Error en el fork del hijo");
                            if (lseek(fd, 0, SEEK_SET) != -1) {
                                while (lockf(fd, F_LOCK, 0) == -1 && errno == EINTR) {
                                }
                                if (lseek(fd, 0, SEEK_SET) != -1 &&
                                    read(fd, &vivos, sizeof(int)) == (ssize_t)sizeof(int) &&
                                    vivos > 1) {
                                    vivos--;
                                    if (lseek(fd, 0, SEEK_SET) != -1) {
                                        write(fd, &vivos, sizeof(int));
                                    }
                                }
                                if (lseek(fd, 0, SEEK_SET) != -1) {
                                    lockf(fd, F_ULOCK, 0);
                                }
                            }
                        } else if (nieto == 0) {
                            // El nuevo hijo (nieto) arranca su propio ciclo igual que un hijo
                            hay_hijos_muertos = 0;
                            morir_sigint = 0;
                            morir_sigterm = 0;

                            if (sigaction(SIGINT, &sa_hijo, NULL) == -1) {
                                perror("Error en sigaction SIGINT del nieto");
                                close(fd);
                                return 1;
                            }

                            sa_hijo.sa_handler = manejadora_sigterm_hijo;
                            if (sigaction(SIGTERM, &sa_hijo, NULL) == -1) {
                                perror("Error en sigaction SIGTERM del nieto");
                                close(fd);
                                return 1;
                            }
                            sa_hijo.sa_handler = manejadora_sigint_hijo;

                            close(fd);
                            fd = open(FICHERO_CONTROL, O_RDWR);
                            if (fd == -1) {
                                perror("Error en open del nieto");
                                return 1;
                            }

                            srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

                            char msg_nac_n[32];
                            int len_nac_n = snprintf(msg_nac_n, sizeof(msg_nac_n), "V(%d)\n", (int)getpid());
                            if (len_nac_n > 0) {
                                if (write(1, msg_nac_n, (size_t)len_nac_n) == -1) {
                                    perror("Error en write de nacimiento del nieto");
                                    close(fd);
                                    return 1;
                                }
                            }

                            while (!morir_sigint && !morir_sigterm) {
                                unsigned int segs_n = (unsigned int)(1 + (rand() % 4));
                                sleep(segs_n);

                                if (morir_sigint || morir_sigterm) {
                                    break;
                                }

                                if (hay_hijos_muertos) {
                                    hay_hijos_muertos = 0;
                                    int estado_n;
                                    pid_t p_m_n;
                                    for (;;) {
                                        p_m_n = waitpid(-1, &estado_n, WNOHANG);
                                        if (p_m_n > 0) {
                                            continue;
                                        }
                                        if (p_m_n == -1 && errno == EINTR) {
                                            continue;
                                        }
                                        break;
                                    }
                                }

                                if (morir_sigint || morir_sigterm) {
                                    break;
                                }

                                if ((rand() % 2) == 0) {
                                    if (kill(getpid(), SIGTERM) == -1) {
                                        perror("Error en kill SIGTERM del nieto");
                                        close(fd);
                                        return 1;
                                    }
                                    break;
                                }
                            }

                            if (morir_sigterm) {
                                if (lseek(fd, 0, SEEK_SET) != -1) {
                                    while (lockf(fd, F_LOCK, 0) == -1) {
                                        if (errno != EINTR) {
                                            break;
                                        }
                                    }
                                    if (lseek(fd, 0, SEEK_SET) != -1 &&
                                        read(fd, &vivos, sizeof(int)) == (ssize_t)sizeof(int) &&
                                        vivos > 1) {
                                        vivos--;
                                        if (lseek(fd, 0, SEEK_SET) != -1) {
                                            write(fd, &vivos, sizeof(int));
                                        }
                                    }
                                    if (lseek(fd, 0, SEEK_SET) != -1) {
                                        lockf(fd, F_ULOCK, 0);
                                    }
                                }
                            }

                            char msg_mue_n[32];
                            int len_mue_n = snprintf(msg_mue_n, sizeof(msg_mue_n), "M(%d)\n", (int)getpid());
                            if (len_mue_n > 0) {
                                if (write(1, msg_mue_n, (size_t)len_mue_n) == -1) {
                                    perror("Error en write de muerte del nieto");
                                    close(fd);
                                    return 1;
                                }
                            }

                            close(fd);
                            return 0;
                        }
                    }
                }

                // PASO 8:
                // Si muere por SIGTERM, decrementar el contador de vivos en el fichero
                if (morir_sigterm) {
                    if (lseek(fd, 0, SEEK_SET) != -1) {
                        while (lockf(fd, F_LOCK, 0) == -1) {
                            if (errno != EINTR) {
                                break;
                            }
                        }
                        if (lseek(fd, 0, SEEK_SET) != -1 &&
                            read(fd, &vivos, sizeof(int)) == (ssize_t)sizeof(int) &&
                            vivos > 1) {
                            vivos--;
                            if (lseek(fd, 0, SEEK_SET) != -1) {
                                write(fd, &vivos, sizeof(int));
                            }
                        }
                        if (lseek(fd, 0, SEEK_SET) != -1) {
                            lockf(fd, F_ULOCK, 0);
                        }
                    }
                }

                // 3. Imprimir M(<PID>) al morir (por SIGINT o por autoenvio de SIGTERM)
                char msg_mue[32];
                int len_mue = snprintf(msg_mue, sizeof(msg_mue), "M(%d)\n", (int)getpid());
                if (len_mue > 0) {
                    if (write(1, msg_mue, (size_t)len_mue) == -1) {
                        perror("Error en write de muerte");
                        close(fd);
                        return 1;
                    }
                }

                close(fd);
                return 0;
            }
        }
    }

    close(fd);
    unlink(FICHERO_CONTROL);

    const char *msg_fin = "Programa acabado correctamente\n";
    if (write(1, msg_fin, strlen(msg_fin)) == -1) {
        perror("Error en write");
        return 1;
    }

    return 0;
}