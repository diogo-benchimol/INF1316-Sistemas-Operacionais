#ifndef SFP_PROTOCOL_H
#define SFP_PROTOCOL_H

// --- Constantes Globais Baseadas no Enunciado ---

// Leitura e escrita são sempre em blocos de 16 bytes
#define SFP_PAYLOAD_SIZE 16
// Um diretório pode ter no máximo 40 nomes
#define SFP_MAX_NAMES_IN_DIR 40
// Tamanho máximo do path. O enunciado sugere não ser longo
#define SFP_MAX_PATH_LEN 512
// Tamanho máximo para o buffer de 'allfilenames' do DL-REP
#define SFP_MAX_ALLFILENAMES_LEN 2048

// --- Códigos de Erro (para status_code ou offset/nrnames) ---
// Usados em campos de resposta como 'offset', 'path_len' ou 'nrnames'
#define SFP_SUCCESS         0
#define SFP_ERR_PERMISSION -1 // Erro de permissão
#define SFP_ERR_NOT_FOUND  -2 // Arquivo ou diretório não encontrado
#define SFP_ERR_OFFSET_OOB -3 // Offset (posição) fora dos limites do arquivo
#define SFP_ERR_IO         -4 // Erro genérico de I/O
#define SFP_ERR_UNKNOWN_MSG -100 // Mensagem desconhecida

// --- Tipos de Mensagem SFP ---

// Usamos um enum para identificar o tipo de operação
typedef enum {
    // File Operations
    SFP_MSG_RD_REQ, // Read Request
    SFP_MSG_RD_REP, // Read Reply
    SFP_MSG_WR_REQ, // Write Request
    SFP_MSG_WR_REP, // Write Reply
    // Directory Operations
    SFP_MSG_DC_REQ, // Directory Create Request
    SFP_MSG_DC_REP, // Directory Create Reply
    SFP_MSG_DR_REQ, // Directory Remove Request
    SFP_MSG_DR_REP, // Directory Remove Reply
    SFP_MSG_DL_REQ, // Directory List Request
    SFP_MSG_DL_REP  // Directory List Reply
} SfpMsgType;

// --- Estrutura para DL-REP (Listar Diretório) ---

// Estrutura para 'fstlstpositions' do DL-REP
// (int, int, int) -> (início, fim, tipo)
typedef struct {
    int start_index; // Posição inicial no char array 'allfilenames'
    int end_index;   // Posição final
    int is_dir;      // 0 para Arquivo (F), 1 para Diretório (D)
} SfpFstLst;

// --- A Mensagem SFP Principal ---

// Esta struct única é usada para TODAS as 10 mensagens.
// Campos não utilizados por um tipo de msg específico serão ignorados.
// Ela também é usada como a struct da MEMÓRIA COMPARTILHADA (shmem)
typedef struct {
    // --- Cabeçalho Comum ---
    SfpMsgType msg_type; // Tipo da mensagem (RD_REQ, RD_REP, etc.)
    int owner;           // Processo de aplicação (A1=1, A2=2, ...)

    // --- Status da Operação (para REPs) ---
    // Usado para códigos de erro (valores negativos)
    // Em RD/WR-REP: será o campo 'offset'
    // Em DC/DR-REP: será o campo 'path_len'
    // Em DL-REP: será o campo 'nrnames'

    // --- Campos de Path e Nome ---
    int path_len;        // strlen(path)
    char path[SFP_MAX_PATH_LEN]; // Ex: "/A1/MyDir/MyFile"

    int name_len;        // strlen(name) - para DC-REQ, DR-REQ
    char name[SFP_MAX_PATH_LEN]; // "dirname" em DC/DR

    // --- Campos de Operações de Arquivo (RD/WR) ---
    int offset;          // 0, 16, 32, etc. (ou código de erro)
    char payload[SFP_PAYLOAD_SIZE]; // Dados (16 bytes)

    // --- Campos de Operações de Diretório (DL-REP) ---
    int nrnames;         // Número de nomes no diretório (ou código de erro)
    SfpFstLst fstlstpositions[SFP_MAX_NAMES_IN_DIR]; // Array de posições
    char allfilenames[SFP_MAX_ALLFILENAMES_LEN];   // Nomes concatenados

} SfpMessage;

#endif // SFP_PROTOCOL_H