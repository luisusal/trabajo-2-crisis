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

/* Estado local de cada proceso: sus hijos directos y las señales pendientes. */
static pid_t hijos[MAX_PROCS];
static int num_hijos = 0;
static volatile sig_atomic_t finalizar = 0;
static volatile sig_atomic_t hay_sigchld = 0;
static int fd_fichero = -1;

/*
 * Las manejadoras solo marcan banderas. La propagacion y la limpieza se hacen
 * fuera de la manejadora, evitando operaciones inseguras durante una señal.
 */
static void manejadora_fin(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        finalizar = 1;
    }
}

static void manejadora_sigchld(int sig)
{
    if (sig == SIGCHLD) {
        hay_sigchld = 1;
    }
}

static void escribir_texto(const char *texto)
{
    size_t longitud = strlen(texto);
    size_t escritos = 0;

    while (escritos < longitud) {
        ssize_t resultado = write(STDOUT_FILENO, texto + escritos,
                                  longitud - escritos);
        if (resultado > 0) {
            escritos += (size_t)resultado;
        } else if (resultado == -1 && errno == EINTR) {
            continue;
        } else {
            return;
        }
    }
}

static void imprimir_estado(char estado)
{
    char texto[32];
    int longitud = snprintf(texto, sizeof(texto), "%c(%d)\n",
                             estado, (int)getpid());

    if (longitud > 0) {
        size_t i = 0;

        while (i < (size_t)longitud) {
            ssize_t resultado = write(STDOUT_FILENO, texto + i,
                                      (size_t)longitud - i);
            if (resultado > 0) {
                i += (size_t)resultado;
            } else if (resultado == -1 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
    }
}

static int bloquear_fichero(int fd)
{
    /* F_LOCK bloquea desde la posicion actual hasta el final del fichero. */
    if (lseek(fd, 0, SEEK_SET) == -1) {
        return -1;
    }

    while (lockf(fd, F_LOCK, 0) == -1) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

static int desbloquear_fichero(int fd)
{
    if (lseek(fd, 0, SEEK_SET) == -1) {
        return -1;
    }
    return lockf(fd, F_ULOCK, 0);
}

static int leer_vivos(int fd, int *vivos)
{
    /* Esta lectura solo se realiza mientras el fichero esta bloqueado. */
    if (lseek(fd, 0, SEEK_SET) == -1) {
        return -1;
    }
    if (read(fd, vivos, sizeof(*vivos)) != (ssize_t)sizeof(*vivos)) {
        return -1;
    }
    return 0;
}

static int escribir_vivos(int fd, int vivos)
{
    if (lseek(fd, 0, SEEK_SET) == -1) {
        return -1;
    }
    if (write(fd, &vivos, sizeof(vivos)) != (ssize_t)sizeof(vivos)) {
        return -1;
    }
    return 0;
}

static int intentar_crear_hijo(int limite, int *es_matriz)
{
    int vivos;
    pid_t pid;
    int fd_viejo;
    sigset_t conjunto;
    sigset_t anterior;
    struct timeval tv;

    sigemptyset(&conjunto);
    sigaddset(&conjunto, SIGINT);
    sigaddset(&conjunto, SIGTERM);
    sigaddset(&conjunto, SIGCHLD);
    sigprocmask(SIG_BLOCK, &conjunto, &anterior);

    /* Una señal recibida puede cancelar la operacion antes de crear el hijo. */
    if (finalizar) {
        sigprocmask(SIG_SETMASK, &anterior, NULL);
        return 0;
    }
    if (bloquear_fichero(fd_fichero) == -1) {
        sigprocmask(SIG_SETMASK, &anterior, NULL);
        return -1;
    }
    /* Leer y modificar el contador forman una unica seccion critica. */
    if (leer_vivos(fd_fichero, &vivos) == -1) {
        desbloquear_fichero(fd_fichero);
        sigprocmask(SIG_SETMASK, &anterior, NULL);
        return -1;
    }
    if (vivos >= limite) {
        desbloquear_fichero(fd_fichero);
        sigprocmask(SIG_SETMASK, &anterior, NULL);
        return 0;
    }

    vivos++;
    if (escribir_vivos(fd_fichero, vivos) == -1) {
        desbloquear_fichero(fd_fichero);
        sigprocmask(SIG_SETMASK, &anterior, NULL);
        return -1;
    }

    if (desbloquear_fichero(fd_fichero) == -1) {
        sigprocmask(SIG_SETMASK, &anterior, NULL);
        return -1;
    }

    /* El hueco se reserva antes de fork para que nadie lo utilice a la vez. */
    pid = fork();
    if (pid == -1) {
        /* Si fork falla, se devuelve el hueco reservado al contador. */
        if (bloquear_fichero(fd_fichero) == 0) {
            if (leer_vivos(fd_fichero, &vivos) == 0 && vivos > 0) {
                vivos--;
                escribir_vivos(fd_fichero, vivos);
            }
            desbloquear_fichero(fd_fichero);
        }
        sigprocmask(SIG_SETMASK, &anterior, NULL);
        return -1;
    }

    if (pid == 0) {
        /*
         * El hijo no reutiliza el descriptor heredado: abre otra vez el mismo
         * fichero con O_RDWR, conserva un descriptor antiguo por seguridad y
         * deja de comportarse como la empresa matriz.
         */
        fd_viejo = fd_fichero;
        fd_fichero = open(FICHERO_CONTROL, O_RDWR);
        num_hijos = 0;
        finalizar = 0;
        hay_sigchld = 0;
        *es_matriz = 0;

        if (fd_fichero == -1) {
            fd_fichero = fd_viejo;
            restar_proceso_vivo();
            close(fd_fichero);
            fd_fichero = -1;
            sigprocmask(SIG_SETMASK, &anterior, NULL);
            imprimir_estado('M');
            _exit(1);
        }
        close(fd_viejo);
        /* Una semilla distinta evita secuencias iguales en procesos hermanos. */
        gettimeofday(&tv, NULL);
        srand((unsigned int)(tv.tv_sec + tv.tv_usec + getpid()));

        sigprocmask(SIG_SETMASK, &anterior, NULL);
        imprimir_estado('V');
        return 2;
    }

    if (num_hijos < MAX_PROCS) {
        hijos[num_hijos] = pid;
        num_hijos++;
    }
    sigprocmask(SIG_SETMASK, &anterior, NULL);
    return 1;
}

static void restar_proceso_vivo(void)
{
    /* Cada proceso que muere disminuye el contador exactamente una vez. */
    int vivos;

    if (bloquear_fichero(fd_fichero) == -1) {
        return;
    }
    if (leer_vivos(fd_fichero, &vivos) == 0 && vivos > 0) {
        vivos--;
        escribir_vivos(fd_fichero, vivos);
    }
    desbloquear_fichero(fd_fichero);
}

static void quitar_hijo(pid_t pid)
{
    int i;

    for (i = 0; i < num_hijos; i++) {
        if (hijos[i] == pid) {
            int j;
            for (j = i; j < num_hijos - 1; j++) {
                hijos[j] = hijos[j + 1];
            }
            num_hijos--;
            return;
        }
    }
}

static void recoger_hijos_no_bloqueante(void)
{
    /* SIGCHLD avisa de que puede haber hijos terminados; waitpid no bloquea. */
    pid_t muerto;
    int estado;

    if (!hay_sigchld) {
        return;
    }

    hay_sigchld = 0;
    do {
        muerto = waitpid(-1, &estado, WNOHANG);
        if (muerto > 0) {
            quitar_hijo(muerto);
        }
    } while (muerto > 0);
}

static void apagar_proceso(int es_matriz)
{
    /*
     * Bloquear las señales evita que una nueva señal interfiera con el cierre
     * recursivo. Primero se avisa a los hijos directos y despues se espera.
     */
    int i;
    pid_t muerto;
    int estado;
    sigset_t conjunto;
    sigset_t anterior;

    sigemptyset(&conjunto);
    sigaddset(&conjunto, SIGINT);
    sigaddset(&conjunto, SIGTERM);
    sigaddset(&conjunto, SIGCHLD);
    sigprocmask(SIG_BLOCK, &conjunto, &anterior);

    /* La quiebra se propaga porque cada hijo repetira este mismo cierre. */
    for (i = 0; i < num_hijos; i++) {
        kill(hijos[i], SIGTERM);
    }

    /* Se espera aqui porque ya no hay señales pendientes ni zombies directos. */
    while (num_hijos > 0) {
        muerto = waitpid(-1, &estado, 0);
        if (muerto > 0) {
            quitar_hijo(muerto);
        } else if (muerto == -1 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }

    /* Solo este proceso se resta: waitpid no modifica el contador compartido. */
    restar_proceso_vivo();
    imprimir_estado('M');

    if (fd_fichero != -1) {
        close(fd_fichero);
        fd_fichero = -1;
    }

    if (es_matriz) {
        unlink(FICHERO_CONTROL);
        escribir_texto("Programa acabado correctamente\n");
    }

    sigprocmask(SIG_SETMASK, &anterior, NULL);
}

int main(int argc, char *argv[])
{
    /* Argumentos, velocidad y tipo de empresa: solo el proceso inicial es matriz. */
    int n_max_procs;
    int velocidad = 0;
    int es_matriz = 1;
    long valor;
    char *fin_ptr;
    struct sigaction sa;
    struct timeval tv;

    /* Comprobar que se recibieron uno o dos argumentos aparte del nombre. */
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Uso: %s n_max_procs [velocidad]\n", argv[0]);
        return 1;
    }

    /* strtol permite rechazar textos y numeros que quedan fuera del intervalo. */
    errno = 0;
    fin_ptr = NULL;
    valor = strtol(argv[1], &fin_ptr, 10);
    if (errno != 0 || *fin_ptr != '\0' ||
        valor < MIN_PROCS || valor > MAX_PROCS) {
        fprintf(stderr, "Error: n_max_procs debe estar entre 3 y 34.\n");
        return 1;
    }
    n_max_procs = (int)valor;

    /* El modo normal es el valor por defecto si no se escribe un tercero argumento. */
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

    /* SIGINT inicia el cierre y SIGTERM indica quiebra o cierre en cascada. */
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = manejadora_fin;
    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    /*
     * SIGCHLD solo levanta una bandera: el padre recogerara a sus hijos desde
     * el bucle principal o desde el cierre bloqueante.
     */
    sa.sa_handler = manejadora_sigchld;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction SIGCHLD");
        return 1;
    }

    /* Solo la matriz crea el fichero con O_TRUNC; el contador inicial es 1. */
    fd_fichero = open(FICHERO_CONTROL, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd_fichero == -1) {
        perror("open");
        return 1;
    }

    if (bloquear_fichero(fd_fichero) == -1 ||
        escribir_vivos(fd_fichero, 1) == -1 ||
        desbloquear_fichero(fd_fichero) == -1) {
        perror("fichero de control");
        close(fd_fichero);
        unlink(FICHERO_CONTROL);
        return 1;
    }

    gettimeofday(&tv, NULL);
    srand((unsigned int)(tv.tv_sec + tv.tv_usec + getpid()));
    imprimir_estado('V');

    /* La matriz y las filiales comparten este ciclo, con decisiones distintas. */
    while (!finalizar) {
        recoger_hijos_no_bloqueante();

        if (finalizar) {
            break;
        }

        /* En modo veloz no se ejecuta sleep, como exige el enunciado. */
        if (velocidad == 0) {
            sleep((unsigned int)(1 + rand() % 4));
        }

        if (finalizar) {
            break;
        }

        /* La matriz intenta crear siempre; una filial decide entre crear o morir. */
        if (es_matriz) {
            if (intentar_crear_hijo(n_max_procs, &es_matriz) == 2) {
                /* El proceso padre ya termino fork; aqui comienza el ciclo del hijo. */
                continue;
            }
        } else if ((rand() % 2) == 0) {
            if (intentar_crear_hijo(n_max_procs, &es_matriz) == 2) {
                /* La filial creada comienza su propio bucle con es_matriz = 0. */
                continue;
            }
        } else {
            /* El 50 % restante representa la quiebra independiente de la filial. */
            finalizar = 1;
        }
    }

    apagar_proceso(es_matriz);
    return 0;
}
