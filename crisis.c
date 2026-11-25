#include <stdio.h>      // generales de c
#include <stdlib.h>     // generales de c
#include <string.h>     //para cadenas de texto
#include <unistd.h>     // llamadas al sistema basicas
#include <errno.h>      // define errno (variable) y codigos de error si falla la llamada del sistema



int main(int argc, char *argv[]){       // le pasamos como argumentos el num de args y un string "normal" o "veloz"
  
    int n_max_procs;
    const char *velocidad = "normal"; //velocidad normal si no lo especifica
    
    //1 comprobar numero de argumentos:
    if(argc < 2 || argc > 3){
        fprintf(stderr, "Uso: %s normal / veloz\n", argv[0]);
        return -1;
    }
    //1 comprobar y validar n_max_procesos
    if(argv[0][1] == '\0'){
        fprintf(stderr, "Error: n_max_procs no puede estar vacio");
        return 1;
    }

    char * fin_ptr = NULL;  // fin de puntero
    errno = 0;
    long valor = strtol(argv[1], &fin_ptr, 10);  //pasamos a long el segundo argumento (le pasamos lo que queremos pasar a long, punteo a char para apuntar donde haya error o final de cadena, en base 10)

    if (errno != 0 || *fin_ptr != '\0' || valor < 3 || valor > 34 ){
        fprintf(stderr, "Error: n_max_procs debe ser un entero entre 3 y 34.");
        return 1;
    }

    // validar el segundo arg opcional
    if (argc == 3) {
        if (strcmp(argv[2], "normal") == 0 || strcmp(argv[2], "veloz") == 0) {
            velocidad = argv[2];
        } else {
            fprintf(stderr, "Error: velocidad debe ser 'normal' o 'veloz'.\n");
            return 1;
        }
    }

    /*/ --- Depuración (comentar cuando ya esté comprobado) ---
    printf("[DEPURACION] argc = %d\n", argc);
    printf("[DEPURACION] argv[1] = '%s' -> n_max_procs = %d\n", argv[1], n_max_procs);
    printf("[DEPURACION] velocidad = '%s'\n", velocidad);
    //---------------------------------------------------------*/

    // Imprimir el número máximo de procesos 
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "Numero maximo de procesos: %d\n", n_max_procs);
    if (len > 0) {
        if (write(1, buf, (size_t)len) == -1) {
            perror("Error en write");
            return 1;
        }
    }

    return 0;
}


// ciclo de vida de cadd proceso (mientras esta vivo)
/*
1-  al nacer debe anunciarlo V(PID) (ejemplo V(1235))
2-  como funciona:
    >    Si es la empresa matriz --> hacer crecer la red creando filiales
        (hijos) mienrtras no pase el limite (n_max_procs)
    >    Si es una filial --> decide que hacer (crear filial o quebrar)
        en modo normal hace pausa entre turno (sleep), en modo veloz no
3-  al morir: avisa a los hijos directos apra que mueran tambien,
    actualiza el contador de procesos vivos DEL FICHERO COMPARTIDO,
    avisa de su muerte con M(PID) (ejemplo M(1235))
    y recoge a los hijos que ya hayan terninado apra no dejar zombies

//-----------------------------------------------------------------


>>> COMO EVITAR QUE SE PISEN ENTRE ELLOS (fichero-señales)
*) contador compartido como aforo: 
    todos los procesos tienen el mismo limite de max procesos a la vez
    necesitan consultar ese numero (contador) en un fichero
    para que 2 procesos no lean a la vez:
    se bloquea el fichero cuando operan
    se desbloquea despeus

*) si un padre abre un fichero y hace 'fork', el hijo hereda el descriptor
    ==> comparten la posicion acutal del cursor
    si uno mueve el cursor, el otro se entera
    conviene que cada hijo abra su propio dsecriptor

*) envez de que un proceso pregunte constantemente si ha muerto un hijo
    se usan señales
    1) señal para que un padre sepa que ha muerto un hijo y recoja su estado
    2) otra para propagar la quiebra/muerte hacia abajo en el arbol genalogico
    3) otra para iniciar cierre ordenado desde el padre

*) ojo con las interrupciones y zombies: pensar en reintentar cuando toque ç
    y recoger  a los hijos muertos siempre
*/
//RESOLVER POR CAPAS
//Capa 1 — Entrada y arranque: validar bien los argumentos (n_max_procs 
//      y normal/veloz), preparar la semilla aleatoria y dejar creado 
//      el fichero inicial con el contador en 1 (solo el padre).
//
//Capa 2 — Salida limpia y creación controlada: comprobar que se 
//      imprime V(PID) y M(PID) con write en el formato exacto y 
//
//      que se respeta el tope máximo de procesos.
//Capa 3 — Árbol de filiales y quiebra en cascada: permitir que
//      los procesos tengan hijos y que, al quebrar uno, el aviso baje
//      por sus descendientes y se limpien los zombies.
//
//Capa 4 — Cierre con CTRL-C y modo veloz: asegurarte de que al 
//      pulsar CTRL-C mueren todos, el padre espera a que no 
//      quede ni uno y, solo entonces, imprime 
//      "Programa acabado correctamente"; 
//      después afinar el modo veloz y revisar que no haya
//      esperas prohibidas.
