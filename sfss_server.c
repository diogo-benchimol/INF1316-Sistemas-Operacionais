#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "sfp_protocol.h"

// --- Headers Adicionais ---
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#define SERVER_PORT 8888
#define BUFFER_SIZE sizeof(SfpMessage)

// Variável global para o diretório raiz
const char* SFSS_ROOT_DIR = NULL;

// --- T2-FIX: Nova Função de Validação de Permissão ---
// Checa se o 'owner' pode acessar o 'path'
// Retorna 1 (true) se permitido, 0 (false) se negado.
int check_permission(int owner, const char* path, int path_len) {
    // Constrói os dois prefixos válidos
    char owner_prefix[10]; // Ex: /A5
    snprintf(owner_prefix, 10, "/A%d", owner);
    char shared_prefix[] = "/A0";

    int owner_prefix_len = strlen(owner_prefix);
    int shared_prefix_len = strlen(shared_prefix);

    int is_owner_path = (strncmp(path, owner_prefix, owner_prefix_len) == 0);
    int is_shared_path = (strncmp(path, shared_prefix, shared_prefix_len) == 0);

    int is_valid = 0;

    // Checa o path do dono (Ex: /A5)
    if (is_owner_path) {
        // Checa se é /A5 (exato) ou /A5/... (subdiretório)
        // Isso previne que /A5 acesse /A50
        if (path_len == owner_prefix_len) { // Exatamente /A5
            is_valid = 1; 
        } else if (path[owner_prefix_len] == '/') { // É /A5/...
            is_valid = 1;
        }
    }

    // Checa o path compartilhado (Ex: /A0)
    if (is_shared_path) {
        // Checa se é /A0 (exato) ou /A0/...
        if (path_len == shared_prefix_len) { // Exatamente /A0
            is_valid = 1;
        } else if (path[shared_prefix_len] == '/') { // É /A0/...
            is_valid = 1;
        }
    }

    return is_valid;
}


// --- Funções de Manipulação ---

void handle_rd_req(const SfpMessage* req, SfpMessage* res) {
    // 1. Inicializa a Resposta
    res->msg_type = SFP_MSG_RD_REP;
    res->owner = req->owner;
    strncpy(res->path, req->path, SFP_MAX_PATH_LEN);
    res->path_len = req->path_len;
    res->offset = req->offset;
    memset(res->payload, 0, SFP_PAYLOAD_SIZE);

    // 2. Validação de Permissões (CORRIGIDO)
    if (!check_permission(req->owner, req->path, strlen(req->path))) {
        printf("Servidor: ERRO (RD) Permissão negada. Owner %d tenta acessar %s\n", req->owner, req->path);
        res->offset = SFP_ERR_PERMISSION; // Retorna erro
        return;
    }

    // 3. Construção do Path Real
    char full_path[SFP_MAX_PATH_LEN + 256];
    snprintf(full_path, sizeof(full_path), "%s%s", SFSS_ROOT_DIR, req->path);

    // 4. Operação de Arquivo
    FILE *file = fopen(full_path, "rb");
    if (file == NULL) {
        printf("Servidor: ERRO (RD) Arquivo não encontrado: %s\n", full_path);
        res->offset = SFP_ERR_NOT_FOUND;
        return;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);

    if (req->offset >= file_size) {
        if (!(file_size == 0 && req->offset == 0)) {
            printf("Servidor: ERRO (RD) Offset fora dos limites. Size: %ld, Offset: %d\n", file_size, req->offset);
            res->offset = SFP_ERR_OFFSET_OOB;
            fclose(file);
            return;
        }
    }

    fseek(file, req->offset, SEEK_SET);
    size_t bytes_read = fread(res->payload, 1, SFP_PAYLOAD_SIZE, file);
    printf("Servidor: (RD) Sucesso. Leu %zu bytes de %s @ offset %d\n", bytes_read, full_path, req->offset);
    fclose(file);
}

void handle_wr_req(const SfpMessage* req, SfpMessage* res) {
    // 1. Inicializa a Resposta
    res->msg_type = SFP_MSG_WR_REP;
    res->owner = req->owner;
    strncpy(res->path, req->path, SFP_MAX_PATH_LEN);
    res->path_len = req->path_len;
    memset(res->payload, 0, SFP_PAYLOAD_SIZE);
    res->offset = req->offset; 

    // 2. Validação de Permissões (CORRIGIDO)
    if (!check_permission(req->owner, req->path, strlen(req->path))) {
        printf("Servidor: ERRO (WR) Permissão negada. Owner %d tenta acessar %s\n", req->owner, req->path);
        res->offset = SFP_ERR_PERMISSION;
        return;
    }

    // 3. Construção do Path Real
    char full_path[SFP_MAX_PATH_LEN + 256];
    snprintf(full_path, sizeof(full_path), "%s%s", SFSS_ROOT_DIR, req->path);

    // 4. Lógica de Remoção
    if (req->offset == 0 && req->payload[0] == '\0') {
        printf("Servidor: (WR) Lógica de REMOÇÃO ativada para %s\n", full_path);
        if (unlink(full_path) == 0) {
            printf("Servidor: (WR) Arquivo removido com sucesso.\n");
            res->offset = 0;
        } else {
            perror("Servidor: ERRO (WR) falha ao remover arquivo");
            res->offset = SFP_ERR_IO;
        }
        return;
    }

    // 5. Lógica de Escrita / Criação
    FILE *file = fopen(full_path, "r+b"); 
    if (file == NULL) {
        printf("Servidor: (WR) Arquivo não existe. Criando %s...\n", full_path);
        file = fopen(full_path, "w+b"); 
        if (file == NULL) {
            perror("Servidor: ERRO (WR) Falha ao criar arquivo");
            res->offset = SFP_ERR_NOT_FOUND;
            return;
        }
    }

    // 6. Lógica de "Buracos"
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    if (req->offset > file_size) {
        printf("Servidor: (WR) Offset > tamanho. Preenchendo buraco de %ld até %d\n", file_size, req->offset);
        char whitespace = 0x20; 
        fseek(file, file_size, SEEK_SET); 
        for (long i = file_size; i < req->offset; i++) {
            if (fwrite(&whitespace, 1, 1, file) != 1) {
                 perror("Servidor: ERRO (WR) Falha ao preencher buraco");
                 res->offset = SFP_ERR_IO;
                 fclose(file);
                 return;
            }
        }
    }

    // 7. Escrita Final
    if (fseek(file, req->offset, SEEK_SET) != 0) {
        perror("Servidor: ERRO (WR) Falha no fseek para o offset");
        res->offset = SFP_ERR_IO;
        fclose(file);
        return;
    }
    size_t bytes_written = fwrite(req->payload, 1, SFP_PAYLOAD_SIZE, file);
    if (bytes_written != SFP_PAYLOAD_SIZE) {
        perror("Servidor: ERRO (WR) Falha ao escrever payload");
        res->offset = SFP_ERR_IO;
    } else {
        printf("Servidor: (WR) Sucesso. Escreveu %zu bytes em %s @ offset %d\n", bytes_written, full_path, req->offset);
    }
    fclose(file);
}

void handle_dc_req(const SfpMessage* req, SfpMessage* res) {
    // 1. Inicializa a Resposta
    res->msg_type = SFP_MSG_DC_REP;
    res->owner = req->owner;

    // 2. Validação de Permissões (CORRIGIDO)
    // A permissão é checada no 'path' base onde o diretório será criado
    if (!check_permission(req->owner, req->path, strlen(req->path))) {
        printf("Servidor: ERRO (DC) Permissão negada. Owner %d tenta criar em %s\n", req->owner, req->path);
        strcpy(res->path, req->path);
        res->path_len = SFP_ERR_PERMISSION; // Retorna erro
        return;
    }

    // 3. Construção do Path Real
    char full_new_path[SFP_MAX_PATH_LEN + 256];
    snprintf(full_new_path, sizeof(full_new_path), "%s%s/%s", 
             SFSS_ROOT_DIR, req->path, req->name);

    // 4. Operação de Criação de Diretório
    if (mkdir(full_new_path, 0755) == 0) {
        printf("Servidor: (DC) Diretório criado: %s\n", full_new_path);
        snprintf(res->path, SFP_MAX_PATH_LEN, "%s/%s", req->path, req->name);
        res->path_len = strlen(res->path);
    } else {
        perror("Servidor: ERRO (DC) falha ao criar diretório");
        strcpy(res->path, req->path);
        res->path_len = SFP_ERR_IO;
    }
}

void handle_dr_req(const SfpMessage* req, SfpMessage* res) {
    // 1. Inicializa a Resposta
    res->msg_type = SFP_MSG_DR_REP;
    res->owner = req->owner;
    strcpy(res->path, req->path); 

    // 2. Validação de Permissões (CORRIGIDO)
    if (!check_permission(req->owner, req->path, strlen(req->path))) {
        printf("Servidor: ERRO (DR) Permissão negada. Owner %d tenta remover de %s\n", req->owner, req->path);
        res->path_len = SFP_ERR_PERMISSION;
        return;
    }

    // 3. Construção do Path Real
    char full_target_path[SFP_MAX_PATH_LEN + 256];
    snprintf(full_target_path, sizeof(full_target_path), "%s%s/%s", 
             SFSS_ROOT_DIR, req->path, req->name);

    // 4. Operação de Remoção
    int status = unlink(full_target_path);
    if (status != 0) {
        status = rmdir(full_target_path);
    }
    if (status == 0) {
        printf("Servidor: (DR) Item removido: %s\n", full_target_path);
        res->path_len = strlen(res->path);
    } else {
        perror("Servidor: ERRO (DR) falha ao remover item");
        res->path_len = SFP_ERR_IO;
    }
}

void handle_dl_req(const SfpMessage* req, SfpMessage* res) {
    // 1. Inicializa a Resposta
    res->msg_type = SFP_MSG_DL_REP;
    res->owner = req->owner;
    res->nrnames = 0;
    memset(res->allfilenames, 0, SFP_MAX_ALLFILENAMES_LEN);
    memset(res->fstlstpositions, 0, sizeof(SfpFstLst) * SFP_MAX_NAMES_IN_DIR);

    // 2. Validação de Permissões (CORRIGIDO)
    if (!check_permission(req->owner, req->path, strlen(req->path))) {
        printf("Servidor: ERRO (DL) Permissão negada. Owner %d tenta listar %s\n", req->owner, req->path);
        res->nrnames = SFP_ERR_PERMISSION;
        return;
    }

    // 3. Construção do Path Real
    char full_path[SFP_MAX_PATH_LEN + 256];
    snprintf(full_path, sizeof(full_path), "%s%s", SFSS_ROOT_DIR, req->path);

    // 4. Operação de Leitura de Diretório
    DIR *d = opendir(full_path);
    if (d == NULL) {
        perror("Servidor: ERRO (DL) falha ao abrir diretório");
        res->nrnames = SFP_ERR_NOT_FOUND;
        return;
    }

    struct dirent *dir_entry;
    int current_name_index = 0;
    int current_char_index = 0;

    while ((dir_entry = readdir(d)) != NULL) {
        if (strcmp(dir_entry->d_name, ".") == 0 || strcmp(dir_entry->d_name, "..") == 0) {
            continue;
        }
        if (current_name_index >= SFP_MAX_NAMES_IN_DIR) {
            break; 
        }

        char* name = dir_entry->d_name;
        int name_len = strlen(name);
        if (current_char_index + name_len >= SFP_MAX_ALLFILENAMES_LEN) {
            break;
        }

        int is_dir = 0;
        char entry_full_path[SFP_MAX_PATH_LEN + 512];
        snprintf(entry_full_path, sizeof(entry_full_path), "%s/%s", full_path, name);

        struct stat st;
        if (stat(entry_full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                is_dir = 1;
            }
        }
        res->fstlstpositions[current_name_index].start_index = current_char_index;
        res->fstlstpositions[current_name_index].end_index = current_char_index + name_len - 1;
        res->fstlstpositions[current_name_index].is_dir = is_dir;

        memcpy(&res->allfilenames[current_char_index], name, name_len);
        current_char_index += name_len;
        current_name_index++;
    }
    closedir(d);
    res->nrnames = current_name_index;
    printf("Servidor: (DL) Sucesso. Listando %d itens de %s\n", res->nrnames, full_path);
}


int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <SFSS-root-dir>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    SFSS_ROOT_DIR = argv[1];
    printf("Servidor SFSS iniciando. Raiz: %s\n", SFSS_ROOT_DIR);

    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    SfpMessage recv_msg;
    SfpMessage send_msg;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Erro ao criar socket");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Erro no bind");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Servidor SFSS aguardando na porta %d...\n", SERVER_PORT);

    while (1) {
        if (recvfrom(sockfd, &recv_msg, BUFFER_SIZE, 0,
                     (struct sockaddr*)&client_addr, &client_len) < 0) {
            perror("Erro no recvfrom");
            continue;
        }

        memset(&send_msg, 0, BUFFER_SIZE);
        send_msg.owner = recv_msg.owner;

        // 5. Processa a Requisição
        switch (recv_msg.msg_type) {
            case SFP_MSG_RD_REQ:
                handle_rd_req(&recv_msg, &send_msg);
                break;
            case SFP_MSG_WR_REQ:
                handle_wr_req(&recv_msg, &send_msg);
                break;
            case SFP_MSG_DC_REQ:
                handle_dc_req(&recv_msg, &send_msg);
                break;
            case SFP_MSG_DR_REQ:
                handle_dr_req(&recv_msg, &send_msg);
                break;
            case SFP_MSG_DL_REQ:
                handle_dl_req(&recv_msg, &send_msg);
                break;
            default:
                printf("Servidor: Recebeu tipo de msg desconhecido: %d\n", recv_msg.msg_type);
                // Prepara uma resposta de erro genérico
                send_msg.msg_type = recv_msg.msg_type + 1; // Resposta genérica
                send_msg.owner = recv_msg.owner;
                send_msg.path_len = SFP_ERR_UNKNOWN_MSG;
        }

        if (sendto(sockfd, &send_msg, BUFFER_SIZE, 0,
                   (struct sockaddr*)&client_addr, client_len) < 0) {
            perror("Erro no sendto");
        }
    }

    close(sockfd);
    return 0;
}