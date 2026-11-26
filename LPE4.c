#include <stdio.h>      // generales de c
#include <stdlib.h>     // generales de c
#include <string.h>     // para cadenas de texto
#include <unistd.h>     // llamadas al sistema basicas
#include <errno.h>      // define errno (variable) y codigos de error si falla la llamada del sistema
#include <time.h>       // para time() en la semilla de srand
#include <fcntl.h>      // open y flags O_RDWR, O_CREAT, O_TRUNC
#include <sys/stat.h>   // permisos de fichero en open

#define FICHERO_CONTROL "crisis.dat" //fichero

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

    // PASO 4:
    // Crear e inicializar el fichero compartido con 1 proceso vivo (el padre)
    int fd = open(FICHERO_CONTROL, O_RDWR | O_CREAT | O_TRUNC, 0644); //con open, creamos (O_CREAT) o abrimos inicializando a 0 (O_TRUNC), el fichero crisis.dat, en modo lectura/escritura (O_RDWR) con permisos 0644--> GUARDA SU DESCRIPTOR EN fd
    if (fd == -1) {
        perror("Error en open");
        return 1;
    }

    if (lseek(fd, 0, SEEK_SET) == -1) {     //coloca el cursor de lectura/escritura (lseek) del fichero (fd) en la posicion 0 (offset) desde el principio (SEEK_SET)
        perror("Error en lseek");           //si falla
        close(fd);                          //cierra fd
        unlink(FICHERO_CONTROL);            //borra el fichero (unlink) para no dejarlo a medias
        return 1;                           //sale con 1
    }
    
            // fd el archivo, F_LOCK bloqueo exclusivo, 0 a todo el archivo (linea de abajo) 
    while (lockf(fd, F_LOCK, 0) == -1) {    //pide el bloqueo (lockf) para que solo un proceso pueda modificarlo
        if (errno != EINTR)                 {//si espera un bloqueo (lockf) pero le llega una señal, salta el error
            perror("Error en lockf F_LOCK");    //si es cualquier error menos EINTR (no es grave)
            close(fd);
            unlink(FICHERO_CONTROL);
            return 1;
        }
    }

    int vivos = 1; // el proceso padri es otro error,e principal es el 1ero
    if (lseek(fd, 0, SEEK_SET) == -1) {
        perror("Error en lseek");
        lockf(fd, F_ULOCK, 0);
        close(fd);
        unlink(FICHERO_CONTROL);
        return 1;
    }
    if (write(fd, &vivos, sizeof(int)) != (ssize_t)sizeof(int)) {   //escribimos lo que haya en vivos (num procs vivos)
        perror("Error en write en fichero");            //comprobamos que se ha introducido un int (y no -1, p.e.)
        lockf(fd, F_ULOCK, 0);      //quitamos lock
        close(fd);                  //cerramos
        unlink(FICHERO_CONTROL);    //borramos fichero
        return 1;
    }

    if (lseek(fd, 0, SEEK_SET) == -1) {     // otro lseek para colocar cursor al inicio
        perror("Error en lseek");
        close(fd);
        unlink(FICHERO_CONTROL);
        return 1;
    }
    if (lockf(fd, F_ULOCK, 0) == -1) {          // quitamos el lock y comprobamos
        perror("Error en lockf F_ULOCK");
        close(fd);
        unlink(FICHERO_CONTROL);
        return 1;
    }

    // Semilla aleatoria con time(NULL) y getpid(), y pausa al azar entre 1 y 4 segundos
    //srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
    srand(unsigned_int)getpid();

    for (;;) { // bucle infinito
        unsigned int segs = (unsigned int)(1 + (rand() % 4));
        sleep(segs);

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
        if (lseek(fd, 0, SEEK_SET) != -1 && read(fd, &vivos, sizeof(int)) == (ssize_t)sizeof(int)) {
            if (vivos < n_max_procs) {
                vivos++;
                if (lseek(fd, 0, SEEK_SET) != -1 && write(fd, &vivos, sizeof(int)) == (ssize_t)sizeof(int)) {
                    hay_hueco = 1;
                }
            }
        }

        if (lseek(fd, 0, SEEK_SET) != -1) {
            lockf(fd, F_ULOCK, 0);
        }

        if (hay_hueco) {
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
    return 0;
}