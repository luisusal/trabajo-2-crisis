/*
 * Guia de LPEs del README:
 * LPE 1: argumentos; LPE 2-3: bucle y pausas aleatorias.
 * LPE 4: fichero contador; LPE 5-6: SIGINT y salida V/M.
 * LPE 7-8: muerte de filiales y SIGTERM; LPE 9: SIGCHLD y waitpid.
 * LPE 10-11: filiales con descendientes y quiebra en cascada.
 * LPE 12-13: modo veloz, bloqueos y respeto estricto del maximo.
 *
 * Las cabeceras siguientes declaran las interfaces POSIX usadas en esos LPEs.
 */
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

/* LPE 10-11: cada proceso recuerda los PID de sus hijos directos. */
static pid_t hijos[MAX_PROCS];
/* LPE 9-11: cantidad de PID actualmente guardados en hijos[]. */
static int num_hijos = 0;
/* LPE 5, 7-8, 11: bandera que hace salir del bucle para iniciar el cierre. */
static int finalizar = 0;
/* LPE 9: la manejadora solo avisa de que hay que llamar a waitpid. */
static int hay_sigchld = 0;
/* LPE 4, 12-13: descriptor del fichero contador de procesos vivos. */
static int fd_fichero = -1;

/*
 * LPE 5 y 8-11: SIGINT inicia cierre por Ctrl-C; SIGTERM inicia la quiebra
 * de una filial. La cascada se ejecuta despues, fuera de la manejadora.
 */
static void manejadora_fin(int sig)
{
    /* Solo las dos señales registradas para esta manejadora solicitan cierre. */
    if (sig == SIGINT || sig == SIGTERM) {
        /* El bucle principal vera esta bandera y llamara al cierre ordenado. */
        finalizar = 1;
    }
}

/* LPE 9: registrar que un hijo termino sin llamar a waitpid desde la señal. */
static void manejadora_sigchld(int sig)
{
    /* SIGCHLD puede agrupar varias terminaciones; luego se recogen todas. */
    if (sig == SIGCHLD) {
        hay_sigchld = 1;
    }
}

/* LPE 6 y 8: escribir mensajes en stdout con write, no con printf. */
static int escribir_pantalla(const char *mensaje)
{
    /* strlen obtiene el numero de bytes del mensaje, sin contar el NUL final. */
    size_t len;

    /* Guardar la longitud para pasarsela a write. */
    len = strlen(mensaje);
    /* Reintentar si write fue interrumpido por una señal. */
    while (write(1, mensaje, len) == -1) {
        /* EINTR no es un error definitivo; otros errores se informan. */
        if (errno != EINTR) {
            perror("write");
            return -1;
        }
    }
    return 0;
}

/* LPE 6-8: formato exacto de nacimiento V(PID) o muerte M(PID). */
static void imprimir_estado_pid(char letra)
{
    /* Buffer local que contiene una linea de salida completa. */
    char buf[32];

    /* Obtener el PID del proceso actual y formatear el estado solicitado. */
    sprintf(buf, "%c(%d)\n", letra, (int)getpid());
    /* Enviar la linea por write mediante la funcion auxiliar. */
    escribir_pantalla(buf);
}

/* LPE 4 y 12-13: bloquear el fichero antes de consultar o cambiar el contador. */
static int bloquear_fichero(int fd)
{
    /* Poner el offset al principio antes de bloquear la region desde ahi. */
    if (lseek(fd, 0, SEEK_SET) == -1) {
        perror("lseek");
        return -1;
    }
    /* Esperar al bloqueo exclusivo; repetir si una señal interrumpe lockf. */
    while (lockf(fd, F_LOCK, 0) == -1) {
        if (errno != EINTR) {
            perror("lockf F_LOCK");
            return -1;
        }
    }
    return 0;
}

/* LPE 4 y 12-13: liberar el bloqueo de la region del fichero. */
static int desbloquear_fichero(int fd)
{
    /* lockf desbloquea desde el offset actual, por eso se reposiciona. */
    if (lseek(fd, 0, SEEK_SET) == -1) {
        perror("lseek");
        return -1;
    }
    /* Informar si el desbloqueo falla. */
    if (lockf(fd, F_ULOCK, 0) == -1) {
        perror("lockf F_ULOCK");
        return -1;
    }
    return 0;
}

/* LPE 4 y 12-13: leer el contador; quien llama debe tener el fichero bloqueado. */
static int leer_fichero(int fd, int *vivos)
{
    /* El valor se almacena desde el comienzo del fichero. */
    if (lseek(fd, 0, SEEK_SET) == -1) {
        perror("lseek");
        return -1;
    }
    /* Leer exactamente un entero; una lectura corta se trata como error. */
    if (read(fd, vivos, sizeof(int)) != (ssize_t)sizeof(int)) {
        perror("read");
        return -1;
    }
    return 0;
}

/* LPE 4 y 12-13: sobrescribir el entero contador; requiere bloqueo previo. */
static int escribir_fichero(int fd, int vivos)
{
    /* Volver al principio para sustituir el valor anterior. */
    if (lseek(fd, 0, SEEK_SET) == -1) {
        perror("lseek");
        return -1;
    }
    /* Guardar el nuevo numero de procesos vivos. */
    if (write(fd, &vivos, sizeof(int)) != (ssize_t)sizeof(int)) {
        perror("write");
        return -1;
    }
    return 0;
}

/* LPE 9 y 11: borrar de la lista local el hijo que ya se ha recogido. */
static void quitar_hijo(pid_t pid)
{
    /* i recorre la lista para encontrar el PID recibido. */
    int i;
    /* j desplaza a la izquierda los elementos posteriores al PID borrado. */
    int j;

    /* Revisar solo las entradas actualmente ocupadas. */
    for (i = 0; i < num_hijos; ++i) {
        /* Comparar PID completos, sin suponer que sean consecutivos. */
        if (hijos[i] == pid) {
            /* Compactar la lista para no dejar huecos. */
            for (j = i; j < num_hijos - 1; ++j) {
                hijos[j] = hijos[j + 1];
            }
            /* Reducir el numero de hijos guardados. */
            num_hijos = num_hijos - 1;
            /* El PID solo aparece una vez, asi que se puede terminar. */
            break;
        }
    }
}

/*
 * LPE 8-9 y 13: este codigo resta un proceso al contador cuando el padre lo
 * ha detectado muerto. En esta version no se resta dentro de la manejadora
 * SIGTERM: la actualizacion se hace al recogerlo con waitpid.
 */
static void restar_proceso_vivo(void)
{
    /* Variable temporal para leer y actualizar el contador compartido. */
    int vivos;

    /* Proteger la operacion leer-modificar-escribir frente a otros procesos. */
    if (bloquear_fichero(fd_fichero) == 0) {
        /* Solo decrementar si la lectura tuvo exito y el valor es positivo. */
        if (leer_fichero(fd_fichero, &vivos) == 0 && vivos > 0) {
            /* El hijo ya ha terminado; liberar una plaza. */
            vivos = vivos - 1;
            /* Persistir el nuevo contador. */
            escribir_fichero(fd_fichero, vivos);
        }
        /* Liberar el fichero incluso si la lectura o escritura fallo. */
        desbloquear_fichero(fd_fichero);
    }
}

int main(int argc, char *argv[])
{
    /* LPE 1-3, 5-13: main configura y ejecuta el ciclo de vida de los procesos. */
    /* Limite maximo solicitado en la linea de ordenes (incluye la matriz). */
    int n_max_procs;
    /* 0 significa normal; 1 significa veloz y por tanto sin sleep. */
    int velocidad; /* 0 = normal, 1 = veloz */
    /* Solo el proceso original mantiene este indicador a 1. */
    int es_matriz;
    /* Copia temporal del contador leido del fichero compartido. */
    int vivos;
    /* Indica que el proceso actual va a intentar crear una filial. */
    int puede_crear;
    /* Recibe el estado del hijo cuando waitpid lo recoge. */
    int estado;
    /* Indice para recorrer la lista local de hijos. */
    int i;
    /* Valor largo intermedio de strtol, antes de comprobar el rango. */
    long val;
    /* Puntero al primer caracter no convertido por strtol. */
    char *fin_ptr;
    /* PID que devuelve fork: cero en el nuevo hijo y PID positivo en el padre. */
    pid_t pid_hijo;
    /* PID que devuelve waitpid al recoger un hijo terminado. */
    pid_t pid_muerto;
    /* Estructura usada para instalar manejadoras de señales con sigaction. */
    struct sigaction sa;
    /* Conjunto de señales que se bloquean durante secciones criticas. */
    sigset_t bloq;
    /* Guarda la mascara de señales anterior para restaurarla despues. */
    sigset_t ant;
    /* Tiempo usado para sembrar rand. */
    struct timeval tv;

    /* LPE 1: comprobar cantidad de argumentos y que n_max_procs no este vacio. */
    if (argc < 2 || argc > 3 || argv[1][0] == '\0') {
        /* argv[0] contiene el nombre usado para ejecutar el programa. */
        fprintf(stderr, "Uso: %s n_max_procs [velocidad]\n", argv[0]);
        /* Un valor distinto de cero indica error al sistema operativo. */
        return 1;
    }

    /* LPE 1: limpiar errno para detectar desbordamiento en strtol. */
    errno = 0;
    /* Inicializar el puntero antes de pedir a strtol que lo actualice. */
    fin_ptr = NULL;
    /* Convertir argv[1] desde texto decimal a entero largo. */
    val = strtol(argv[1], &fin_ptr, 10);
    /* Rechazar texto mal formado, caracteres extra y valores fuera de 3..34. */
    if (errno != 0 || *fin_ptr != '\0' ||
        val < MIN_PROCS || val > MAX_PROCS) {
        /* El mensaje explica el intervalo admitido. */
        fprintf(stderr, "Error: n_max_procs debe estar entre 3 y 34.\n");
        /* No continuar con argumentos invalidos. */
        return 1;
    }
    /* Convertir a int una vez que ya se comprobo el intervalo. */
    n_max_procs = (int)val;

    /* LPE 1 y 12: normal es el modo por defecto si no se da el tercer argumento. */
    velocidad = 0;
    /* El segundo argumento opcional selecciona el modo de ejecucion. */
    if (argc == 3) {
        /* La velocidad normal mantiene las pausas aleatorias. */
        if (strcmp(argv[2], "normal") == 0) {
            velocidad = 0;
        /* La velocidad veloz elimina las llamadas a sleep del bucle. */
        } else if (strcmp(argv[2], "veloz") == 0) {
            velocidad = 1;
        /* Cualquier otra palabra es un error de uso. */
        } else {
            fprintf(stderr, "Error: velocidad debe ser normal o veloz.\n");
            return 1;
        }
    }
