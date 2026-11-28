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
static int fd_fichero = -1;        // descriptor propio del fichero en cada proceso

static void manejadora_sigint_padre(int sig)
{
    if (sig == SIGINT) { // ctrl c padre
        finalizar = 1;
    }
}

static void manejadora_sigint_hijo(int sig)
{
    if (sig == SIGINT) { // ctrl c hijo
        morir_sigint = 1;
    }
}

static void manejadora_sigterm_hijo(int sig)
{
    if (sig == SIGTERM) { // muerte aleatoria
        morir_sigterm = 1;
    }
}

static void manejadora_sigchld(int sig)
{
    if (sig == SIGCHLD) { // se autoenvia al suceder SIGTERM
        hay_hijos_muertos = 1;
    }
}

static int escribir_stdout(const char *texto, size_t len)
{
    while (write(1, texto, len) == -1) {
        if (errno != EINTR) {
            perror("Error en write");
            return -1;
        }
    }
    return 0;
}

static void recoger_hijos_no_bloqueante(void)
{
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
}

static int ajustar_vivos_fichero(int fd, int n_max_procs, int delta, int comprobar_tope, int *ok)
{
    int vivos = 0;
    if (ok != NULL) {
        *ok = 0;
    }
    if (fd < 0) {
        return -1;
    }
    if (lseek(fd, 0, SEEK_SET) == -1) {
        perror("Error en lseek");
        return -1;
    }
    while (lockf(fd, F_LOCK, 0) == -1) {
        if (errno != EINTR) {
            perror("Error en lockf F_LOCK");
            return -1;
        }
    }

    if (lseek(fd, 0, SEEK_SET) != -1 &&
        read(fd, &vivos, sizeof(int)) == (ssize_t)sizeof(int)) {
        if (delta > 0) { //si nace (delta=1)
            if (!comprobar_tope || vivos < n_max_procs) {
                vivos += delta; //incrementamos contador
                if (lseek(fd, 0, SEEK_SET) != -1 &&
                    write(fd, &vivos, sizeof(int)) == (ssize_t)sizeof(int) &&
                    ok != NULL) {
                    *ok = 1; //se ha modificado el contador
                }
            }
        } else if (delta < 0) { // si muere (delta = -1)
            if (vivos > 1) {
                vivos += delta; // decrementamos contador
                if (lseek(fd, 0, SEEK_SET) != -1 &&
                    write(fd, &vivos, sizeof(int)) == (ssize_t)sizeof(int) &&
                    ok != NULL) {
                    *ok = 1; //se ha modificado el contador
                }
            }
        }
    }

    if (lseek(fd, 0, SEEK_SET) != -1) {
        if (lockf(fd, F_ULOCK, 0) == -1) {
            perror("Error en lockf F_ULOCK");
            return -1;
        }
    }
    return 0;
}

static int crear_y_arrancar_hijo(int n_max_procs);

static int ejecutar_ciclo_hijo(int n_max_procs)
{
    hay_hijos_muertos = 0; //inicializas flags a 0
    morir_sigint = 0;
    morir_sigterm = 0;

    struct sigaction sa_hijo; //hace el struct

    sigemptyset(&sa_hijo.sa_mask); //limpia señales basura

    sa_hijo.sa_flags = 0; // si el proceso está bloqueado en una llamada como sleep o lock, y llega una señal registrada (como SIGINT) esa llamada puede interrumpirse y devolver -1 con errno == EINTR, Gracias a eso, el bucle puede comprobar la bandera finalizar y salir sin quedarse esperando a terminar la pausa

    sa_hijo.sa_handler = manejadora_sigint_hijo; // sirve para indicarle a la estructura struct sigaction que funcion debe ejecutarse (manejadora) cuando llegue la señal 

    if (sigaction(SIGINT, &sa_hijo, NULL) == -1) {
        perror("Error en sigaction SIGINT del hijo");
        if (fd_fichero >= 0) {
            close(fd_fichero);
            fd_fichero = -1;
        }
        return 1;
    }

    sa_hijo.sa_handler = manejadora_sigterm_hijo; //que funcion manejadora debe ejecutarse

    if (sigaction(SIGTERM, &sa_hijo, NULL) == -1) {
        perror("Error en sigaction SIGTERM del hijo");
        if (fd_fichero >= 0) {
            close(fd_fichero);
            fd_fichero = -1;
        }
        return 1;
    }

    // Cerrar descriptor heredado y abrir con O_RDWR (sin O_TRUNC)
    if (fd_fichero >= 0) {
        close(fd_fichero);
        fd_fichero = -1;
    }
    fd_fichero = open(FICHERO_CONTROL, O_RDWR);
    if (fd_fichero == -1) {
        perror("Error en open del hijo");
        return 1;
    }

    // Semilla propia del proceso combinando tiempo y su PID
    //srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
    srand((unsigned int)getpid());

    // Imprimir V(<PID>) justo al nacer
    char msg_nac[32];
    int len_nac = snprintf(msg_nac, sizeof(msg_nac), "V(%d)\n", (int)getpid());
    if (len_nac > 0 && escribir_stdout(msg_nac, (size_t)len_nac) == -1) {  //write
        close(fd_fichero);
        fd_fichero = -1;
        return 1;
    }

    // Bucle del hijo: duerme 1..4 s, recoge hijos muertos, sortea morir
    // por SIGTERM o, si no muere, crea su propio hijo usando la misma funcion
    while (!morir_sigint && !morir_sigterm) {
        unsigned int segs_hijo = (unsigned int)(1 + (rand() % 4));
        sleep(segs_hijo);

        if (morir_sigint || morir_sigterm) {
            break;
        }

        recoger_hijos_no_bloqueante(); //no zombis (waitpid)

        if (morir_sigint || morir_sigterm) {
            break;
        }

        int azar_muerte = rand() % 2; // 0 -> muere, 1 -> no muere
        if (azar_muerte == 0) {
            if (kill(getpid(), SIGTERM) == -1) {
                perror("Error en kill SIGTERM");
                close(fd_fichero);
                fd_fichero = -1;
                return 1;
            }
            break;
        }

        if (!morir_sigint && !morir_sigterm) {
            if (crear_y_arrancar_hijo(n_max_procs) == 1) {
                if (fd_fichero >= 0) {
                    close(fd_fichero);
                    fd_fichero = -1;
                }
                return 1;
            }
        }
    }

    // Si muere por SIGTERM, decrementar el contador de vivos en el fichero
    if (morir_sigterm && fd_fichero >= 0) {
        ajustar_vivos_fichero(fd_fichero, n_max_procs, -1, 0, NULL); //delta, tope, *ok
    }

    // Imprimir M(<PID>) al morir
    char msg_mue[32];
    int len_mue = snprintf(msg_mue, sizeof(msg_mue), "M(%d)\n", (int)getpid());
    if (len_mue > 0 && escribir_stdout(msg_mue, (size_t)len_mue) == -1) { //write
        if (fd_fichero >= 0) {
            close(fd_fichero);
            fd_fichero = -1;
        }
        return 1;
    }

    if (fd_fichero >= 0) {
        close(fd_fichero);
        fd_fichero = -1;
    }
    return 0;
}

static int crear_y_arrancar_hijo(int n_max_procs)
{
    int hay_hueco = 0;

    if (finalizar || morir_sigint || morir_sigterm || fd_fichero < 0) {
        return 0;
    }

    if (ajustar_vivos_fichero(fd_fichero, n_max_procs, 1, 1, &hay_hueco) == -1) { //incrementamos
        return -1;
    }

    if (hay_hueco && !finalizar && !morir_sigint && !morir_sigterm) { //vivo
        pid_t pid = fork();
        if (pid == -1) {
            perror("Error en el fork");
            ajustar_vivos_fichero(fd_fichero, n_max_procs, -1, 0, NULL); //error : contador disminuye
            return -1;
        } else if (pid == 0) {
            return ejecutar_ciclo_hijo(n_max_procs); // hace hijos
        }
    }
    return 0;
}

int main(int argc, char *argv[])
{
    int n_max_procs;
    const char *velocidad = "normal";

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

    // Imprimir el numero maximo de procesos
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "Numero maximo de procesos: %d\n", n_max_procs);
    if (len > 0 && escribir_stdout(buf, (size_t)len) == -1) {
        return 1;
    }

    // PASO 5: Registrar SIGINT para el padre
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = manejadora_sigint_padre;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Error en sigaction SIGINT");
        return 1;
    }

    // PASO 9: Registrar SIGCHLD para recoger hijos muertos sin bloquearse
    struct sigaction sa_chld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = 0;
    sa_chld.sa_handler = manejadora_sigchld;
    if (sigaction(SIGCHLD, &sa_chld, NULL) == -1) {
        perror("Error en sigaction SIGCHLD");
        return 1;
    }

    // PASO 4: Crear e inicializar el fichero compartido con 1 proceso vivo (el padre)
    fd_fichero = open(FICHERO_CONTROL, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd_fichero == -1) {
        perror("Error en open");
        return 1;
    }

    int vivos = 1;
    if (lseek(fd_fichero, 0, SEEK_SET) == -1) {
        perror("Error en lseek");
        close(fd_fichero);
        fd_fichero = -1;
        unlink(FICHERO_CONTROL);
        return 1;
    }
    while (lockf(fd_fichero, F_LOCK, 0) == -1) {
        if (errno != EINTR) {
            perror("Error en lockf F_LOCK");
            close(fd_fichero);
            fd_fichero = -1;
            unlink(FICHERO_CONTROL);
            return 1;
        }
    }
    if (lseek(fd_fichero, 0, SEEK_SET) == -1 ||
        write(fd_fichero, &vivos, sizeof(int)) != (ssize_t)sizeof(int)) {
        perror("Error en write en fichero");
        lseek(fd_fichero, 0, SEEK_SET);
        lockf(fd_fichero, F_ULOCK, 0);
        close(fd_fichero);
        fd_fichero = -1;
        unlink(FICHERO_CONTROL);
        return 1;
    }
    if (lseek(fd_fichero, 0, SEEK_SET) == -1 || lockf(fd_fichero, F_ULOCK, 0) == -1) {
        perror("Error en lockf F_ULOCK");
        close(fd_fichero);
        fd_fichero = -1;
        unlink(FICHERO_CONTROL);
        return 1;
    }

    //srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
    srand((unsigned int)getpid())

    while (!finalizar) {
        unsigned int segs = (unsigned int)(1 + (rand() % 4));
        sleep(segs);

        if (finalizar) {
            break;
        }

        recoger_hijos_no_bloqueante(); // recoger zombies

        if (!finalizar) {
            crear_y_arrancar_hijo(n_max_procs); //proceso de hijos
        }
    }

    if (fd_fichero >= 0) {
        close(fd_fichero);
        fd_fichero = -1;
    }
    unlink(FICHERO_CONTROL);

    const char *msg_fin = "Programa acabado correctamente\n";
    if (escribir_stdout(msg_fin, strlen(msg_fin)) == -1) {
        return 1;
    }

    return 0;
}