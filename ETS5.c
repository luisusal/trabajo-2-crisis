#define __USE_POSIX

#include <stdio.h>      // generales de c
#include <stdlib.h>     // generales de c
#include <string.h>     // para cadenas de texto
#include <unistd.h>     // llamadas al sistema basicas
#include <errno.h>      // define errno (variable) y codigos de error si falla la llamada del sistema
#include <time.h>       // para time() en la semilla de srand
#include <fcntl.h>      // open y flags O_RDWR, O_CREAT, O_TRUNC
#include <sys/stat.h>   // permisos de fichero en open
#include <signal.h>     // sigaction y SIGINT

#define FICHERO_CONTROL "crisis.dat"
//static: restringe la visibilidad de la variable al archivo actual y le da duración estática
static int finalizar = 0; // se pone a 1 al recibir SIGINT (CTRL-C)
static void manejadora_sigint(int sig){
    if (sig == SIGINT) {
        finalizar = 1;
    }
}

static int finalizar = 0; // variable de control. se pone a 1 al recibir SIGINT (CTRL-C)

static void manejadora_sigint(int sig){
    (void)sig;      // este parametro no se usa.
    finalizar = 1;  // marca el flag a 1 cuando recibe SIGINT.
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

    struct sigaction sa; //estructura de tipo sigaction llamada sa

    sigemptyset(&sa.sa_mask); //sigempty deja vacío, sin señales un conjunto de señales sigset_t. campo sa_mask de tipo sigset_t dentro de la estructura sa, indica que señales adicionales quedan bloqueadas mientras corre la manejadora
    sa.sa_flags = 0; // si el proceso está bloqueado en una llamada como sleep o lock, y llega una señal registrada (como SIGINT) esa llamada puede interrumpirse y devolver -1 con errno == EINTR, Gracias a eso, el bucle puede comprobar la bandera finalizar y salir sin quedarse esperando a terminar la pausa
    sa.sa_handler = manejadora_sigint; // sirve para indicarle a la estructura struct sigaction que funcion debe ejecutarse (manejadora) cuando llegue la señal SIGINT

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("Error en sigaction");
        return 1;
    }

    struct sigaction sa;        // de signal.h

    //sa.sa_mask es un conjunto de señales (sigset_t) que quedan bloqueadas mientras se ejecuta la manejadora
    sigemptyset(&sa.sa_mask);   // inicializa la mascara dejandola vacia. 
    sa.sa_flags = 0;                    //no activamos banderas especiales
    sa.sa_handler = manejadora_sigint;  //asigna manejadora_sigint para que sepa que funcion ejectar

    if (sigaction(SIGINT, &sa, NULL) == -1) {//sigaction cambia el comportamiento de SIGINT, pasandole el puntero al strct sigaction, y no guardamos config de la señal anterior (NULL)
        perror("Error en sigaction");
        return 1;
    }   //cambiamos lo que hace sigint con sigaction: ahora antes de matar el proceso, da un mensaje de error

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

    // Semilla aleatoria con time(NULL) y getpid(), y pausa al azar entre 1 y 4 segundos
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());

    while (!finalizar) { // bucle hasta recibir SIGINT (CTRL-C)
        unsigned int segs = (unsigned int)(1 + (rand() % 4));
        sleep(segs);

        if (finalizar) {
            break;
        }

        // Bloquear fichero, comprobar si hay hueco y reservar antes de hacer fork
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
            pid_t pid = fork(); // crear proceso

            if (pid == -1) { // si es -1 es que falla la creacion del hijo
                perror("Error en el fork");
                // Devolver la reserva si fork fallo
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
            } else if (pid == 0) { // si es 0 estamos dentro del proceso hijo
                // Cerrar descriptor heredado y abrir con O_RDWR (sin O_TRUNC)
                close(fd);
                fd = open(FICHERO_CONTROL, O_RDWR);
                if (fd == -1) {
                    perror("Error en open del hijo");
                    return 1;
                }
                // proceso hijo se queda bloqueado en pause() de momento
                for (;;) {
                    pause();
                }
            }
        }
    }

    close(fd);
    unlink(FICHERO_CONTROL);

    const char *msg_fin = "Programa acabado correctamente\n"; //puntero a cte de char qe apunta al mensaje
    size_t len_fin = strlen(msg_fin);   //tamaño del string en len_fin
    while (write(1, msg_fin, len_fin) == -1) {     //write, sobre la salida standar 1, envia el mensage msg_fin, de len_fin bytes
        if (errno != EINTR) {       //comprobamos que no haya error
            perror("Error en write de finalizacion");
            return 1;
        }
    }

    return 0;
}