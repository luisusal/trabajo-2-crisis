sigemptyset  inicializa un conjunto de señales dejándolo completamente vacío,  sin ninguna señal marcada en su interior.

Para qué la usas en struct sigaction: 
indica qué señales adicionales quieres que el sistema bloquee temporalmente mientras se está ejecutando tu función manejadora. Como una variable local struct sigaction sa; nace con basura en memoria, llamas a sigemptyset(&sa.sa_mask); para asegurarte de que la máscara empieza limpia y no bloquea ninguna otra señal por accidente.

Qué devuelve: devuelve 0 si todo ha ido bien, o -1 si hubiera algún error.