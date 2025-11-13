**KernelSim T2 — Simulador de Microkernel + Servidor de Arquivos (SFSS)**

Este projeto implementa o Trabalho 2 de Sistemas Operacionais, consistindo em:

Um microkernel (KernelSim_T2) capaz de:

* Gerenciar processos de aplicação (A1–A5)

* Realizar escalonamento Round Robin

* Lidar com bloqueio/desbloqueio por chamadas de sistema

* Comunicar-se com um servidor de arquivos via UDP

* Usar memória compartilhada (shm) para retornos de I/O

* Tratar interrupções (IRQ0/IRQ1/IRQ2) geradas por um "interrupt controller"

Um Servidor de Arquivos Simples (SFSS) (sfss_server) que:

* Implementa uma API minimalista para operações de arquivo e diretório

* Recebe requisições SFP (Simple File Protocol) via UDP

* Valida permissões de acesso: cada app só acessa /A{id} ou /A0

* Lê/escreve arquivos em blocos de 16 bytes

* Cria e remove diretórios

* Lista entradas de diretórios com metadados

* Retorna respostas SFP compatíveis com o protocolo definido no sfp_protocol.h

**Estrutura do Projeto**
.
├── KernelSim_T2.c        # Código do microkernel
├── sfss_server.c         # Servidor de arquivos simples
├── sfp_protocol.h        # Estruturas e constantes do protocolo SFP
├── Makefile              # Compilação, limpeza e execução
└── sfss_root/            # Diretório raiz do SFSS
    ├── A0/               # Áreas de trabalho dos apps
    ├── A1/
    ├── A2/
    ├── A3/
    ├── A4/
    └── A5/


As pastas A0..A5 são criadas automaticamente pelo make.

**Execução**
(comandos com o makefile)

1. Compilar tudo
make

2. Iniciar apenas o servidor SFSS
make server

3. Iniciar apenas o kernel
make run

OBS.: É recomendável executar o trabalho em 3 terminais diferentes, um com o kernel (make run),
outro com o server (make server) e outro para voltar com os processos após uma snapshot
(kill -CONT [pid]). Dessa forma, os logs não se misturam, facilitando a compreensão

4. Limpeza
4.1. Remover arquivos e diretórios criados dentro de sfss_root
make clean-root

Este comando mantém as pastas A0..A5, mas remove:
* file.txt
* arquivos .bin
* quaisquer diretórios criados dinamicamente pelos apps (ex.: newDir_A3_10)

4.2. Remover executáveis
make clean

**Visão Geral do Funcionamento**
1. Kernel

* Cria 5 processos de aplicação (A1–A5)

* Cada app envia syscalls aleatórias de:

* READ (arquivo)

* WRITE (arquivo)

* ADD / REM (diretório)

* LISTDIR (diretório)

** Cada syscall:

* É enviada ao SFSS via UDP (SFP_REQ)

* Bloqueia o app (state = BLOCKED)

* Quando o SFSS responde, o kernel:

* Copia a resposta na shmem do app

* Desbloqueia o processo (state = READY)

* Coloca-o de volta na fila READY

* IRQ0 → troca de contexto (quantum)

* IRQ1 → resposta de operações de arquivo (file I/O)

* IRQ2 → resposta de operações de diretório (dir I/O)

2. SFSS Server

* Recebe mensagens SFP via UDP

* Ação depende do msg_type:

* RD_REQ → lê 16 bytes do arquivo

* WR_REQ → escreve 16 bytes

* DC_REQ → cria diretório

* DR_REQ → remove arquivo/diretório

* DL_REQ → lista entradas de diretório

** Valida permissões:

* app A3 só pode acessar /A3/* ou /A0/*

* Envia respostas SFP via UDP para o kernel

3. Protocolo SFP

Definido em sfp_protocol.h, inclui:

* Tipo da mensagem (request/response)

* Offset

* Path

*Payload de 16 bytes

* Listagens de diretório

* Códigos de erro unificados

**Log e Depuração**

* O kernel imprime todo o fluxo de execução em stdout/stderr.

** Pressionar Ctrl+C no kernel gera um snapshot do estado:

* PC de cada processo

* READY queue

* BLOCKED queue

* File-Q e Dir-Q

* Processos terminados

**Informações Técnicas Importantes**

* Comunicação kernel ↔ SFSS via UDP (porta 8888)

** Comunicação kernel ↔ apps via:

* pipe() para sinais de TICK e mensagens

* shared memory (shmget/shmat) para respostas de I/O

* Escalonamento Round Robin com quantum de 0.5s

* Tamanho fixo de leitura/escrita: 16 bytes

* Processos terminam após MAX_PC = 20 instruções

**Conclusão**

* O projeto implementa com sucesso um microkernel funcional capaz de:

* Escalonar processos

* Bloqueá-los e desbloqueá-los por syscalls

* Comunicar-se com um servidor remoto

* Manter consistência das operações com shmem

* Manipular diretórios e arquivos via protocolo SFP

* Ele é totalmente automatizado pelo Makefile e pode ser repetido inúmeras vezes com limpeza garantida.
