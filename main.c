/**
 * @file main.c
 * @brief Gerenciamento principal do sistema da Caixa de Amostras.
 * @details Este arquivo contém a lógica principal de operação do núcleo 0 (core 0),
 * incluindo a máquina de estados, controle de hardware (sensores, atuadores, display),
 * e a comunicação com o núcleo 1 para tarefas de rede.
 */

// ===================================================================================
// --- Inclusões de Bibliotecas e Módulos ---
// ===================================================================================

#include "configura_geral.h" // Arquivo de configuração geral do projeto (ex: pinos)
#include <string.h>
#include <math.h>

// Bibliotecas do SDK do Pico
#include "pico/multicore.h"      // Para gerenciamento dos dois núcleos do RP2040
#include "pico/cyw43_arch.h"     // Para controle do chip Wi-Fi (CYW43)
#include "hardware/gpio.h"       // Controle de pinos de I/O de propósito geral
#include "hardware/pwm.h"        // Para controle de PWM (ex: LED RGB, servo)
#include "hardware/i2c.h"        // Para comunicação I2C (ex: display, sensor de cor)
#include "pico/time.h"           // Funções de tempo e timers

// Drivers dos módulos de hardware específicos do projeto
#include "display.h"   // Driver para o display OLED
#include "mqtt_lwip.h" // Funções para comunicação MQTT sobre a pilha lwIP
#include "matriz.h"    // Driver para a matriz de LED
#include "keypad.h"    // Driver para o teclado matricial
#include "tcs34725.h"  // Driver para o sensor de cor TCS34725
#include "rgb_led.h"   // Driver para o LED RGB
#include "buzzer.h"    // Driver para o buzzer
#include "servo.h"     // Driver para o servo motor
#include "feedback.h"  // Funções de feedback ao usuário (visual e sonoro)
#include "aht10.h"      // Driver para o sensor de temperatura e umidade

// ===================================================================================
// --- Definições e Constantes Globais ---
// ===================================================================================

#define SERVO_MOVE_DURATION_US 500000     // Duração do movimento do servo (0.5 segundos)
#define TIMEOUT_SENHA_S 15                // Tempo limite para digitar a senha (15 segundos)
#define TEMPO_AUTO_TRAVA_S 20             // Tempo para o sistema trancar automaticamente (20 segundos)
#define DISPLAY_UPDATE_INTERVAL_US 1000000 // Intervalo para atualizar o display (1 segundo)

// ===================================================================================
// --- Estruturas de Dados e Enumerações ---
// ===================================================================================

/**
 * @brief Estrutura para um timer não-bloqueante.
 * @details Permite verificar se um período de tempo passou sem pausar a execução do código.
 */
typedef struct {
    bool ativo;             // Indica se o timer está em contagem.
    absolute_time_t inicio; // Marcação de tempo do início da contagem.
    uint64_t duracao_us;    // Duração do timer em microssegundos.
} TimerNaoBloqueante;

/**
 * @brief Estrutura para controlar o efeito de pulso do LED RGB.
 * @details Usado para criar uma animação suave de brilho crescente e decrescente.
 */
typedef struct {
    bool ativo;             // Indica se o efeito está em execução.
    absolute_time_t inicio; // Marcação de tempo do início do efeito para cálculo do seno.
    uint8_t r, g, b;        // Cor base do pulso.
} EfeitoPulso;

/**
 * @brief Enumeração para os modos de mensagem temporária no display.
 * @details Usado para exibir uma mensagem por um tempo limitado (ex: erro, sucesso)
 * e depois retornar ao estado normal, sobrepondo-se à tela do modo atual.
 */
typedef enum {
    MSG_MODE_NONE,          // Nenhuma mensagem temporária ativa.
    MSG_MODE_TIMEOUT,       // Mensagem de tempo esgotado.
    MSG_MODE_ACCESS_DENIED, // Mensagem de acesso negado.
    MSG_MODE_ADMIN_SUCCESS, // Mensagem de sucesso no modo admin.
    MSG_MODE_ADMIN_ERROR,   // Mensagem de erro no modo admin.
    MSG_MODE_ADMIN_CANCELLED// Mensagem de operação cancelada.
} ModoMensagemTemporaria;


/**
 * @brief Estrutura principal que contém todo o estado da fechadura.
 * @details Centraliza todas as variáveis que definem o comportamento atual do sistema.
 */
typedef struct {
    volatile enum ModoOperacao modo_atual; // O modo de operação atual da máquina de estados.
    enum CorDetectada cor_ativa;          // A cor do cartão que está sendo processado.
    bool status_aberto;                   // `true` se a fechadura está aberta, `false` se fechada.
    bool modo_foi_inicializado;           // Flag para executar rotinas de inicialização de um modo apenas uma vez.

    char senha_digitada[5];               // Buffer para armazenar a senha digitada pelo usuário.
    int digitos_count;                    // Contador de quantos dígitos da senha já foram inseridos.

    // Timers não-bloqueantes para diversas funcionalidades
    TimerNaoBloqueante timer_servo;             // Controla a duração do movimento do servo.
    TimerNaoBloqueante timer_timeout_senha;     // Controla o tempo limite para inserir a senha.
    TimerNaoBloqueante timer_auto_trava;        // Controla o tempo para fechar a caixa automaticamente.
    TimerNaoBloqueante timer_display_update;    // Controla a frequência de atualização do display.
    TimerNaoBloqueante timer_msg_temporaria;    // Controla a duração de mensagens de erro/sucesso.
    TimerNaoBloqueante timer_alarme_beep;       // Controla os bipes do alarme de temperatura.
    TimerNaoBloqueante timer_leitura_sensor;    // Controla a frequência de leitura do sensor AHT10.
    TimerNaoBloqueante timer_heartbeat;         // Controla o envio de mensagens "vivas" para o MQTT.

    // Flags de estado para animações e alarmes
    bool alarme_temperatura_ativo;        // `true` se o alarme de temperatura está disparado.
    bool animacao_erro_ativa;             // `true` se a animação de erro está em execução.
    bool animacao_timeout_ativa;          // `true` se a animação de timeout está em execução.
    bool animacao_fechando_ativa;         // `true` se a animação de fechamento está em execução.
    bool animacao_sucesso_ativa;          // `true` se a animação de sucesso está em execução.

    // Estruturas de estado para efeitos visuais
    EfeitoPulso efeito_pulso;             // Controla o efeito de pulso do LED RGB.
    ModoMensagemTemporaria modo_msg_ativo; // Indica qual mensagem temporária está ativa.

} EstadoFechadura;

// ===================================================================================
// --- Variáveis Globais ---
// ===================================================================================

// Senhas padrão para cada cor. Podem ser alteradas em tempo de execução pelo modo admin.
char SENHA_VERDE[5] = "1337";
char SENHA_VERMELHA[5] = "8008";
char SENHA_AZUL[5] = "4242";

// Instância única da estrutura de estado, acessível por todas as funções.
static EstadoFechadura fechadura;
// Estrutura para armazenar os dados lidos do sensor AHT10.
static aht10_data_t dados_sensor;

// ===================================================================================
// --- Protótipos de Funções ---
// ===================================================================================

// Funções utilitárias de timer
void timer_iniciar(TimerNaoBloqueante *timer, uint64_t duracao_us);
bool timer_expirou(TimerNaoBloqueante *timer);

// Funções de controle de feedback visual
void led_iniciar_pulso(uint8_t r, uint8_t g, uint8_t b);
void led_parar_pulso();
void reset_visual_state();

// Funções de comunicação e ação
void solicitar_publicacao_mqtt(enum MQTT_MSG_TYPE tipo_msg, enum CorDetectada cor);
void verificar_fifo(void);
void acionar_fechamento();
void acionar_abertura();

// Funções de lógica de negócio
enum CorDetectada detectar_cor_cartao(tcs34725_color_data_t colors);
void mudar_modo(enum ModoOperacao novo_modo);
void exibir_mensagem_temporaria(ModoMensagemTemporaria tipo_msg, uint32_t duracao_ms);

// Funções de gerenciamento de modos (máquina de estados)
void handle_modo_espera();
void handle_modo_aguarda_senha();
void handle_modo_aberto();
void handle_admin_aguardando_cartao();
void handle_admin_aguardando_nova_senha();

// Funções de inicialização
void inicia_hardware();
void inicia_core1();

// Função principal do núcleo 1
void funcao_wifi_nucleo1();


// ===================================================================================
// --- Implementação das Funções ---
// ===================================================================================

/**
 * @brief Inicia ou reinicia um timer não-bloqueante.
 * @param timer Ponteiro para a estrutura do timer.
 * @param duracao_us Duração desejada em microssegundos.
 */
void timer_iniciar(TimerNaoBloqueante *timer, uint64_t duracao_us) {
    timer->ativo = true;
    timer->inicio = get_absolute_time();
    timer->duracao_us = duracao_us;
}

/**
 * @brief Verifica se um timer não-bloqueante já expirou.
 * @param timer Ponteiro para a estrutura do timer.
 * @return `true` se o tempo passou, `false` caso contrário. Se o timer estiver inativo, retorna `false`.
 */
bool timer_expirou(TimerNaoBloqueante *timer) {
    if (!timer->ativo) return false;
    if (absolute_time_diff_us(timer->inicio, get_absolute_time()) >= timer->duracao_us) {
        timer->ativo = false; // Desativa o timer automaticamente ao expirar.
        return true;
    }
    return false;
}

/**
 * @brief Inicia o efeito de pulso no LED RGB com uma cor específica.
 */
void led_iniciar_pulso(uint8_t r, uint8_t g, uint8_t b) {
    fechadura.efeito_pulso.ativo = true;
    fechadura.efeito_pulso.inicio = get_absolute_time();
    fechadura.efeito_pulso.r = r;
    fechadura.efeito_pulso.g = g;
    fechadura.efeito_pulso.b = b;
}

/**
 * @brief Para o efeito de pulso e desliga o LED RGB.
 */
void led_parar_pulso() {
    fechadura.efeito_pulso.ativo = false;
    rgb_led_set_color(0, 0, 0); // Garante que o LED apague.
}

/**
 * @brief Envia um comando para o núcleo 1 (via FIFO) para publicar uma mensagem MQTT.
 * @param tipo_msg O tipo de mensagem a ser enviada (definido na enum `MQTT_MSG_TYPE`).
 * @param cor A cor associada ao evento, se houver.
 */
void solicitar_publicacao_mqtt(enum MQTT_MSG_TYPE tipo_msg, enum CorDetectada cor) {
    // Empacota o tipo da mensagem e a cor em um único valor de 16 bits.
    uint16_t valor = (uint16_t)((tipo_msg & 0xFF) | ((cor & 0xFF) << 8));
    // Empacota o comando e o valor em uma única palavra de 32 bits para a FIFO.
    uint32_t pacote = (FIFO_CMD_PUBLICAR_MQTT << 16) | valor;
    multicore_fifo_push_blocking(pacote);
}

/**
 * @brief Função centralizada para mudar o modo de operação da máquina de estados.
 * @details Garante que estados antigos sejam limpos (timers, senhas, animações)
 * antes de entrar em um novo modo.
 * @param novo_modo O modo para o qual o sistema deve transicionar.
 */
void mudar_modo(enum ModoOperacao novo_modo) {
    if (fechadura.modo_atual == novo_modo) return; // Evita transições desnecessárias

    fechadura.modo_atual = novo_modo;
    fechadura.modo_foi_inicializado = false; // Sinaliza que a rotina de inicialização do novo modo deve ser executada.

    // Reseta estados que não devem persistir entre modos.
    fechadura.digitos_count = 0;
    memset(fechadura.senha_digitada, 0, sizeof(fechadura.senha_digitada));

    // Para qualquer animação ou efeito visual para garantir um estado limpo.
    led_parar_pulso();
    fechadura.animacao_erro_ativa = false;
    fechadura.animacao_timeout_ativa = false;
    fechadura.animacao_fechando_ativa = false;
    fechadura.animacao_sucesso_ativa = false;
}

/**
 * @brief Verifica a fila FIFO por comandos vindos do núcleo 1.
 * @details Atualmente, só lida com o comando para mudar o estado (ex: entrar em modo admin).
 */
void verificar_fifo(void) {
    if (multicore_fifo_rvalid()) {
        uint32_t pacote = multicore_fifo_pop_blocking();
        uint16_t comando = pacote >> 16;
        uint16_t valor = pacote & 0xFFFF;

        if (comando == FIFO_CMD_MUDAR_ESTADO) {
            mudar_modo((enum ModoOperacao)valor); // Usa a função centralizada para a mudança.
        }
    }
}

/**
 * @brief Reseta o estado visual (LEDs, Matriz) para um estado neutro (desligado).
 */
void reset_visual_state() {
    rgb_led_set_color(0, 0, 0);
    matriz_limpar();
    led_parar_pulso();
    // Garante que todas as flags de animação sejam desativadas.
    fechadura.animacao_erro_ativa = false;
    fechadura.animacao_timeout_ativa = false;
    fechadura.animacao_fechando_ativa = false;
    fechadura.animacao_sucesso_ativa = false;
}


/**
 * @brief Inicia a sequência de fechamento da fechadura.
 */
void acionar_fechamento() {
    display_show_message(NULL, "Fechado", NULL);
    fechadura.animacao_fechando_ativa = true;   // Inicia animação visual de fechamento.
    rgb_led_set_color(PWM_MAX_DUTY, 0, 0);      // LED vermelho indica fechamento.
    servo_start_move(0);                        // Move o servo para a posição 'fechado'.
    timer_iniciar(&fechadura.timer_servo, SERVO_MOVE_DURATION_US);
    fechadura.status_aberto = false;
    solicitar_publicacao_mqtt(MSG_STATUS_SISTEMA_FECHADO, COR_NENHUMA);
    mudar_modo(MODO_ESPERA); // Retorna ao modo de espera.
}

/**
 * @brief Inicia a sequência de abertura da fechadura após sucesso.
 */
void acionar_abertura() {
    feedback_tocar_sucesso();                   // Toca som de sucesso.
    fechadura.animacao_sucesso_ativa = true;    // Inicia animação visual de sucesso.
    matriz_limpar();
    rgb_led_set_color(0, PWM_MAX_DUTY, 0);      // LED verde indica sucesso/aberto.
    display_show_message("ACESSO LIBERADO", "Bem-vindo!", NULL);
    servo_start_move(150);                      // Move o servo para a posição 'aberto'.
    timer_iniciar(&fechadura.timer_servo, SERVO_MOVE_DURATION_US);
    fechadura.status_aberto = true;
    // Inicia o timer para trancar a caixa automaticamente.
    timer_iniciar(&fechadura.timer_auto_trava, (uint64_t)TEMPO_AUTO_TRAVA_S * 1000000);
    solicitar_publicacao_mqtt(MSG_STATUS_SISTEMA_ABERTO, COR_NENHUMA);
    solicitar_publicacao_mqtt(MSG_LOG_ACESSO_OK, fechadura.cor_ativa);
    mudar_modo(MODO_ABERTO); // Entra no modo aberto.
}

/**
 * @brief Exibe uma mensagem temporária na tela com feedback sonoro e visual.
 * @details Esta função sobrepõe a tela do modo atual por um tempo determinado.
 * @param tipo_msg O tipo de mensagem a ser exibida (erro, timeout, etc.).
 * @param duracao_ms A duração da mensagem em milissegundos.
 */
void exibir_mensagem_temporaria(ModoMensagemTemporaria tipo_msg, uint32_t duracao_ms) {
    fechadura.modo_msg_ativo = tipo_msg;
    timer_iniciar(&fechadura.timer_msg_temporaria, duracao_ms * 1000);

    switch(tipo_msg) {
        case MSG_MODE_TIMEOUT:
            feedback_tocar_timeout();
            display_show_message("OPERAÇÃO EXPIRADA", "Tempo esgotado", NULL);
            solicitar_publicacao_mqtt(MSG_LOG_EVENTO_TIMEOUT_SENHA, fechadura.cor_ativa);
            fechadura.animacao_timeout_ativa = true;
            break;
        case MSG_MODE_ACCESS_DENIED:
            feedback_tocar_erro();
            display_show_message("ACESSO NEGADO", "Senha Incorreta", NULL);
            solicitar_publicacao_mqtt(MSG_LOG_ACESSO_FALHA, fechadura.cor_ativa);
            fechadura.animacao_erro_ativa = true;
            break;
        case MSG_MODE_ADMIN_SUCCESS:
            {
                char log_buffer[50];
                sprintf(log_buffer, "Senha para Cartao %s alterada.", fechadura.cor_ativa == COR_VERDE ? "Verde" : (fechadura.cor_ativa == COR_VERMELHA ? "Vermelho" : "Azul"));
                display_show_message("SUCESSO!", "Senha Salva.", log_buffer);
                feedback_tocar_sucesso();
                solicitar_publicacao_mqtt(MSG_LOG_ADMIN_SENHA_ALTERADA, fechadura.cor_ativa);
                fechadura.animacao_sucesso_ativa = true;
            }
            break;
        case MSG_MODE_ADMIN_ERROR:
            display_show_message("ERRO", "Senha 4 digitos!", NULL);
            feedback_tocar_erro();
            fechadura.animacao_erro_ativa = true;
            break;
        case MSG_MODE_ADMIN_CANCELLED:
            display_show_message("--- MODO ADMIN ---", "Operacao Cancelada", "");
            solicitar_publicacao_mqtt(MSG_LOG_OPERACAO_CANCELADA, COR_NENHUMA);
            break;
        default:
            break;
    }
}

/**
 * @brief Analisa os dados do sensor de cor e determina a cor predominante.
 * @param colors Dados lidos do sensor TCS34725.
 * @return A cor detectada (enum `CorDetectada`) ou `COR_NENHUMA`.
 */
enum CorDetectada detectar_cor_cartao(tcs34725_color_data_t colors) {
    const int CLEAR_THRESHOLD = 70; // Limiar de intensidade para evitar detecções falsas no escuro.
    if (colors.clear < CLEAR_THRESHOLD) return COR_NENHUMA;

    // Lógica para diferenciar as cores com base na proporção entre os canais R, G e B.
    // Estes valores podem precisar de ajuste fino dependendo da iluminação e dos cartões.
    if ((colors.green > colors.red * 1.8) && (colors.green > colors.blue * 1.8)) return COR_VERDE;
    if ((colors.red > colors.green * 2.0) && (colors.red > colors.blue * 2.0)) return COR_VERMELHA;
    if ((colors.blue > colors.green * 1.5) && (colors.blue > colors.red * 2.0)) return COR_AZUL;
    
    return COR_NENHUMA;
}


// -----------------------------------------------------------------------------------
// --- Funções Handler da Máquina de Estados ---
// -----------------------------------------------------------------------------------

/**
 * @brief Lógica para o MODO_ESPERA.
 * @details Aguarda a aproximação de um cartão colorido e exibe dados dos sensores.
 */
void handle_modo_espera() {
    // Bloco de inicialização: executado apenas uma vez ao entrar neste modo.
    if (!fechadura.modo_foi_inicializado) {
        reset_visual_state();
        solicitar_publicacao_mqtt(MSG_STATUS_AGUARDANDO_CARTAO, COR_NENHUMA);
        led_iniciar_pulso(0, 0, 255); // Inicia um pulso azul para indicar "pronto".
        fechadura.modo_foi_inicializado = true;
    }

    // Atualiza o display periodicamente com informações de status e sensores.
    if (timer_expirou(&fechadura.timer_display_update) || !fechadura.timer_display_update.ativo) {
        char linha1_display[25], linha2_display[25], linha3_display[25];
        sprintf(linha1_display, "Aproxime o cartao");
        sprintf(linha2_display, "Temp: %.1f C", dados_sensor.temperature);
        sprintf(linha3_display, "Umid: %.1f %%", dados_sensor.humidity);
        display_show_message(linha1_display, linha2_display, linha3_display);
        timer_iniciar(&fechadura.timer_display_update, DISPLAY_UPDATE_INTERVAL_US);
    }

    // Lê o sensor de cor continuamente.
    tcs34725_color_data_t colors;
    tcs34725_read_colors(i2c0, &colors);
    enum CorDetectada cor_detectada = detectar_cor_cartao(colors);

    // Se uma cor válida for detectada, muda para o modo de aguardar senha.
    if (cor_detectada != COR_NENHUMA) {
        fechadura.cor_ativa = cor_detectada;
        solicitar_publicacao_mqtt(MSG_STATUS_CARTAO_LIDO, fechadura.cor_ativa);
        timer_iniciar(&fechadura.timer_timeout_senha, (uint64_t)TIMEOUT_SENHA_S * 1000000);
        mudar_modo(MODO_AGUARDA_SENHA);
    }
}

/**
 * @brief Lógica para o MODO_AGUARDA_SENHA.
 * @details Espera o usuário digitar a senha no teclado numérico.
 */
void handle_modo_aguarda_senha() {
    // Bloco de inicialização.
    if (!fechadura.modo_foi_inicializado) {
        reset_visual_state();
        solicitar_publicacao_mqtt(MSG_STATUS_AGUARDANDO_SENHA, fechadura.cor_ativa);
        rgb_led_set_color(PWM_MAX_DUTY, PWM_MAX_DUTY, 0); // LED amarelo indica "aguardando input".
        fechadura.modo_foi_inicializado = true;
    }

    // Atualiza o display periodicamente com a senha digitada e o tempo restante.
    if (timer_expirou(&fechadura.timer_display_update) || !fechadura.timer_display_update.ativo) {
        int64_t diff_us = absolute_time_diff_us(fechadura.timer_timeout_senha.inicio, get_absolute_time());
        int tempo_restante = TIMEOUT_SENHA_S - (diff_us / 1000000);
        if (tempo_restante < 0) tempo_restante = 0;
        
        char linha1[20], linha3[20];
        // Personaliza a mensagem de acordo com a cor do cartão.
        switch (fechadura.cor_ativa) {
            case COR_VERDE: sprintf(linha1, "Senha (Verde):"); break;
            case COR_VERMELHA: sprintf(linha1, "Senha (Vermelho):"); break;
            case COR_AZUL: sprintf(linha1, "Senha (Azul):"); break;
            default: sprintf(linha1, "Digite a senha:"); break;
        }
        sprintf(linha3, "Tempo: %ds", tempo_restante);
        display_show_message(linha1, fechadura.senha_digitada, linha3);
        timer_iniciar(&fechadura.timer_display_update, DISPLAY_UPDATE_INTERVAL_US);
    }

    // Verifica se o tempo para digitar a senha expirou.
    if (timer_expirou(&fechadura.timer_timeout_senha)) {
        exibir_mensagem_temporaria(MSG_MODE_TIMEOUT, 4000);
        mudar_modo(MODO_ESPERA);
        return;
    }

    // Processa a entrada do teclado.
    char tecla = keypad_get_key();
    if (tecla != '\0') {
        buzzer_play_tone(1500, 50); // Feedback sonoro para cada tecla pressionada.
        
        if (tecla == '#') { // Tecla de confirmação
            bool senha_valida = false;
            // Valida a senha digitada contra a senha armazenada para a cor ativa.
            switch (fechadura.cor_ativa) {
                case COR_VERDE: if (strcmp(fechadura.senha_digitada, SENHA_VERDE) == 0) senha_valida = true; break;
                case COR_VERMELHA: if (strcmp(fechadura.senha_digitada, SENHA_VERMELHA) == 0) senha_valida = true; break;
                case COR_AZUL: if (strcmp(fechadura.senha_digitada, SENHA_AZUL) == 0) senha_valida = true; break;
                default: senha_valida = false; break;
            }

            if (senha_valida) {
                acionar_abertura(); // Senha correta, abre a fechadura.
            } else {
                exibir_mensagem_temporaria(MSG_MODE_ACCESS_DENIED, 4000); // Senha incorreta.
                mudar_modo(MODO_ESPERA);
            }
        } else if (tecla == '*') { // Tecla de cancelamento
            solicitar_publicacao_mqtt(MSG_LOG_OPERACAO_CANCELADA, fechadura.cor_ativa);
            mudar_modo(MODO_ESPERA);
        } else if (fechadura.digitos_count < 4) { // Adiciona dígito à senha
            fechadura.senha_digitada[fechadura.digitos_count++] = tecla;
            fechadura.senha_digitada[fechadura.digitos_count] = '\0'; // Mantém o final nulo.
        }
    }
}

/**
 * @brief Lógica para o MODO_ABERTO.
 * @details Mantém a fechadura aberta e exibe um contador para o fechamento automático.
 */
void handle_modo_aberto() {
    // Bloco de inicialização.
    if (!fechadura.modo_foi_inicializado) {
        reset_visual_state();
        rgb_led_set_color(0, PWM_MAX_DUTY, 0); // LED verde indica "aberto".
        fechadura.modo_foi_inicializado = true;
    }
    
    // Atualiza o display com o tempo restante para o travamento automático.
    if (timer_expirou(&fechadura.timer_display_update) || !fechadura.timer_display_update.ativo) {
        int64_t diff_us = absolute_time_diff_us(fechadura.timer_auto_trava.inicio, get_absolute_time());
        int tempo_restante = TEMPO_AUTO_TRAVA_S - (diff_us / 1000000);
        if (tempo_restante < 0) tempo_restante = 0;
        
        char linha2_buffer[25];
        sprintf(linha2_buffer, "Travando em: %ds", tempo_restante);
        display_show_message("Sistema Aberto", linha2_buffer, NULL);
        timer_iniciar(&fechadura.timer_display_update, DISPLAY_UPDATE_INTERVAL_US);
    }
    
    // Verifica se o timer de auto-travamento expirou.
    if (timer_expirou(&fechadura.timer_auto_trava)) {
        solicitar_publicacao_mqtt(MSG_LOG_EVENTO_AUTO_LOCK, COR_NENHUMA);
        acionar_fechamento();
    }
}

/**
 * @brief Lógica para o modo admin, aguardando um cartão para configurar.
 * @details Este modo é ativado via comando MQTT.
 */
void handle_admin_aguardando_cartao() {
    // Bloco de inicialização.
    if (!fechadura.modo_foi_inicializado) {
        reset_visual_state();
        display_show_message("--- MODO ADMIN ---", "Aproxime o cartao", "a ser configurado");
        solicitar_publicacao_mqtt(MSG_STATUS_MODO_ADMIN, COR_NENHUMA);
        led_iniciar_pulso(255, 0, 255); // Pulso roxo para indicar modo admin.
        fechadura.modo_foi_inicializado = true;
    }

    // Aguarda a detecção de um cartão colorido.
    tcs34725_color_data_t colors;
    tcs34725_read_colors(i2c0, &colors);
    enum CorDetectada cor_detectada_admin = detectar_cor_cartao(colors);
    
    if (cor_detectada_admin != COR_NENHUMA) {
        fechadura.cor_ativa = cor_detectada_admin;
        mudar_modo(MODO_ADMIN_AGUARDANDO_NOVA_SENHA); // Avança para o próximo passo do modo admin.
    }
}

/**
 * @brief Lógica para o modo admin, aguardando a digitação da nova senha.
 */
void handle_admin_aguardando_nova_senha() {
    // Bloco de inicialização.
    if (!fechadura.modo_foi_inicializado) {
        reset_visual_state();
        char linha1_buffer[20];
        sprintf(linha1_buffer, "Nova Senha (%s):", fechadura.cor_ativa == COR_VERDE ? "Verde" : (fechadura.cor_ativa == COR_VERMELHA ? "Vermelho" : "Azul"));
        display_show_message("--- MODO ADMIN ---", linha1_buffer, "");
        rgb_led_set_color(PWM_MAX_DUTY, PWM_MAX_DUTY, 0); // LED amarelo.
        fechadura.modo_foi_inicializado = true;
    }
    
    // Atualiza o display com a nova senha sendo digitada.
    if (timer_expirou(&fechadura.timer_display_update) || !fechadura.timer_display_update.ativo) {
        char linha1_buffer[20];
        sprintf(linha1_buffer, "Nova Senha (%s):", fechadura.cor_ativa == COR_VERDE ? "Verde" : (fechadura.cor_ativa == COR_VERMELHA ? "Vermelho" : "Azul"));
        display_show_message("--- MODO ADMIN ---", linha1_buffer, fechadura.senha_digitada);
        timer_iniciar(&fechadura.timer_display_update, DISPLAY_UPDATE_INTERVAL_US);
    }
    
    // Processa a entrada do teclado.
    char tecla = keypad_get_key();
    if (tecla != '\0') {
        buzzer_play_tone(1500, 50);
        
        if (tecla == '#') { // Confirmação da nova senha.
            if (fechadura.digitos_count == 4) { // Verifica se a senha tem 4 dígitos.
                // Copia a nova senha para a variável global correspondente.
                switch (fechadura.cor_ativa) {
                    case COR_VERDE: strcpy(SENHA_VERDE, fechadura.senha_digitada); break;
                    case COR_VERMELHA: strcpy(SENHA_VERMELHA, fechadura.senha_digitada); break;
                    case COR_AZUL: strcpy(SENHA_AZUL, fechadura.senha_digitada); break;
                    default: break;
                }
                exibir_mensagem_temporaria(MSG_MODE_ADMIN_SUCCESS, 5000); // Exibe sucesso.
                mudar_modo(MODO_ESPERA); // Retorna ao modo de espera.
            } else {
                exibir_mensagem_temporaria(MSG_MODE_ADMIN_ERROR, 4000); // Exibe erro.
                // Reseta a digitação para uma nova tentativa sem sair do modo.
                fechadura.digitos_count = 0;
                memset(fechadura.senha_digitada, 0, sizeof(fechadura.senha_digitada));
            }
        } else if (tecla == '*') { // Cancelamento.
            exibir_mensagem_temporaria(MSG_MODE_ADMIN_CANCELLED, 3000);
            mudar_modo(MODO_ESPERA);
        } else if (fechadura.digitos_count < 4) { // Adiciona dígito.
            fechadura.senha_digitada[fechadura.digitos_count++] = tecla;
            fechadura.senha_digitada[fechadura.digitos_count] = '\0';
        }
    }
}


// -----------------------------------------------------------------------------------
// --- Funções de Inicialização e Loop Principal ---
// -----------------------------------------------------------------------------------

/**
 * @brief Inicializa todo o hardware conectado ao microcontrolador.
 */
void inicia_hardware() {
    stdio_init_all();
    display_init();
    rgb_led_init();
    buzzer_init();
    servo_init();
    matriz_init();
    matriz_limpar();
    keypad_init();

    // Inicialização do I2C para os sensores.
    i2c_init(i2c0, 100 * 1000);
    gpio_set_function(TCS34725_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(TCS34725_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(TCS34725_SDA_PIN);
    gpio_pull_up(TCS34725_SCL_PIN);

    // Verifica se o sensor de cor foi inicializado corretamente.
    if (!tcs34725_init(i2c0)) {
        display_show_message("ERRO FATAL", "TCS34725 falhou!", NULL);
        rgb_led_set_color(PWM_MAX_DUTY, 0, 0); // LED vermelho de erro.
        while (true) { tight_loop_contents(); } // Trava a execução.
    }
    // Verifica se o sensor de temp/umidade foi inicializado corretamente.
    if (!aht10_init(i2c0)) {
        display_show_message("ERRO FATAL", "AHT10 falhou!", NULL);
        rgb_led_set_color(PWM_MAX_DUTY, 0, 0); // LED vermelho de erro.
        while (true) { tight_loop_contents(); } // Trava a execução.
    }
    
    // Inicializa a estrutura de estado da fechadura com valores padrão.
    memset(&fechadura, 0, sizeof(EstadoFechadura));
    fechadura.modo_atual = MODO_ESPERA;
    fechadura.modo_foi_inicializado = false;
    fechadura.alarme_temperatura_ativo = false;
    fechadura.modo_msg_ativo = MSG_MODE_NONE;

    // Faz uma leitura inicial dos sensores para ter dados válidos desde o início.
    aht10_read_data(i2c0, &dados_sensor); 
}

/**
 * @brief Lança a função de controle de Wi-Fi e MQTT para o núcleo 1.
 */
void inicia_core1() {
    multicore_launch_core1(funcao_wifi_nucleo1);
}

/**
 * @brief Ponto de entrada principal do programa (executado no núcleo 0).
 */
int main() {
    inicia_hardware();

    // --- Sequência de Conexão (Handshake com Núcleo 1) ---
    display_show_message("Rede", "Conectando Wi-Fi...", NULL);
    rgb_led_set_color(40000, 15000, 0); // Laranja piscando durante conexão Wi-Fi.
    inicia_core1();

    // Aguarda a confirmação de conexão Wi-Fi do núcleo 1.
    uint32_t fifo_response;
    while (!multicore_fifo_rvalid()) { tight_loop_contents(); }
    fifo_response = multicore_fifo_pop_blocking();
    if ((fifo_response >> 16) != FIFO_CMD_WIFI_CONECTADO || (fifo_response & 0xFFFF) != WIFI_STATUS_SUCCESS) {
        display_show_message("ERRO FATAL", "Falha na conexao", "Wi-Fi");
        rgb_led_set_color(PWM_MAX_DUTY, 0, 0);
        while(true) { tight_loop_contents(); } // Trava em caso de falha.
    }
    
    display_show_message("Rede", "Wi-Fi Conectado!", NULL);
    rgb_led_set_color(0, PWM_MAX_DUTY, 0);
    sleep_ms(1500);

    // Aguarda a confirmação de conexão MQTT do núcleo 1.
    display_show_message("Rede", "Conectando Broker", "MQTT...");
    rgb_led_set_color(0, 20000, 40000); // Ciano durante conexão MQTT.
    while(true) {
        if (multicore_fifo_rvalid()) {
            fifo_response = multicore_fifo_pop_blocking();
            if ((fifo_response >> 16) == FIFO_CMD_MQTT_CONECTADO) break;
        }
        tight_loop_contents();
    }

    display_show_message("Caixa de Amostras", "Sistema Pronto", NULL);
    buzzer_tocar_melodia_sucesso();
    sleep_ms(2500);
    reset_visual_state();
    
    // ===============================================================================
    // --- Loop Principal de Operação (Core 0) ---
    // ===============================================================================
    while (true) {
        verificar_fifo(); // Verifica por comandos do núcleo 1.

        // Gerenciador de mensagens temporárias.
        // Se uma mensagem está ativa, ele tem prioridade sobre a lógica do modo atual.
        if (fechadura.modo_msg_ativo != MSG_MODE_NONE) {
            if (timer_expirou(&fechadura.timer_msg_temporaria)) {
                fechadura.modo_msg_ativo = MSG_MODE_NONE;
                fechadura.modo_foi_inicializado = false; // Força reinicialização da tela do modo atual.
                reset_visual_state();
            }
        } else {
            // Executa a lógica do modo atual da máquina de estados.
            switch (fechadura.modo_atual) {
                case MODO_ESPERA:                     handle_modo_espera(); break;
                case MODO_AGUARDA_SENHA:              handle_modo_aguarda_senha(); break;
                case MODO_ABERTO:                     handle_modo_aberto(); break;
                case MODO_ADMIN_AGUARDANDO_CARTAO:    handle_admin_aguardando_cartao(); break;
                case MODO_ADMIN_AGUARDANDO_NOVA_SENHA:handle_admin_aguardando_nova_senha(); break;
                default:
                    mudar_modo(MODO_ESPERA); // Modo de segurança: se o estado for inválido, volta para espera.
                    break;
            }
        }

        // --- Leitura Periódica de Sensores ---
        if (timer_expirou(&fechadura.timer_leitura_sensor) || !fechadura.timer_leitura_sensor.ativo) {
            if (aht10_read_data(i2c0, &dados_sensor)) {
                // Envia dados para o núcleo 1 publicar via MQTT.
                uint16_t temp_int = (uint16_t)(dados_sensor.temperature * 100.0f);
                multicore_fifo_push_blocking((FIFO_CMD_PUB_SENSOR_TEMP << 16) | temp_int);
                uint16_t umid_int = (uint16_t)(dados_sensor.humidity * 100.0f);
                multicore_fifo_push_blocking((FIFO_CMD_PUB_SENSOR_UMID << 16) | umid_int);
            }
            timer_iniciar(&fechadura.timer_leitura_sensor, 10000000); // Lê a cada 10 segundos.
        }
        
        // --- Lógica do Alarme de Temperatura ---
        bool temp_fora_da_faixa = (dados_sensor.temperature < TEMP_MIN_SEGURA || dados_sensor.temperature > TEMP_MAX_SEGURA);
        if (temp_fora_da_faixa && !fechadura.alarme_temperatura_ativo) {
            fechadura.alarme_temperatura_ativo = true;
            solicitar_publicacao_mqtt(MSG_ALARM_TEMP_ON, COR_NENHUMA);
        } else if (!temp_fora_da_faixa && fechadura.alarme_temperatura_ativo) {
            fechadura.alarme_temperatura_ativo = false;
            solicitar_publicacao_mqtt(MSG_ALARM_TEMP_OFF, COR_NENHUMA);
            fechadura.timer_alarme_beep.ativo = false;
            buzzer_stop_beep();
            reset_visual_state(); // Limpa o visual do alarme imediatamente.
            fechadura.modo_foi_inicializado = false; // Força reinicialização visual do modo atual.
        }

        // --- Gerenciamento Centralizado de Feedback Visual ---
        // Verifica se alguma animação ou alarme prioritário está ativo.
        bool visual_override = fechadura.animacao_erro_ativa || fechadura.animacao_timeout_ativa || 
                               fechadura.animacao_fechando_ativa || fechadura.animacao_sucesso_ativa ||
                               fechadura.alarme_temperatura_ativo;

        // Atualiza o estado das animações. Elas se desativam sozinhas quando terminam.
        if (fechadura.animacao_erro_ativa) {
            if (feedback_visual_erro_update()) fechadura.animacao_erro_ativa = false;
        } else if (fechadura.animacao_timeout_ativa) {
            if (feedback_visual_timeout_update()) fechadura.animacao_timeout_ativa = false;
        } else if (fechadura.animacao_fechando_ativa) {
            if (feedback_visual_fechando_update()) fechadura.animacao_fechando_ativa = false;
        } else if (fechadura.animacao_sucesso_ativa) {
            if (matriz_animacao_sucesso_update()) fechadura.animacao_sucesso_ativa = false;
        }
        
        // Lógica visual para o alarme de temperatura (alta prioridade).
        if (fechadura.alarme_temperatura_ativo) {
            if (timer_expirou(&fechadura.timer_alarme_beep) || !fechadura.timer_alarme_beep.ativo) {
                buzzer_play_tone(3000, 100);
                timer_iniciar(&fechadura.timer_alarme_beep, 1000000); // Bip a cada 1 segundo.
            }
            // Animação de pulso vermelho na matriz e LED.
            float brilho = (sinf((float)to_ms_since_boot(get_absolute_time()) * (float)M_PI / 1500.0f) + 1.0f) / 2.0f;
            matriz_desenhar_ponto_central((uint8_t)(255 * brilho), 0, 0);
            rgb_led_set_color(PWM_MAX_DUTY, 0, 0);
        }

        // --- Lógica Visual Padrão (só executa se não houver um override visual) ---
        if (!visual_override && fechadura.modo_msg_ativo == MSG_MODE_NONE) {
            // Efeito de pulso para o modo de espera e admin.
            if (fechadura.efeito_pulso.ativo) {
                float tempo_ms = absolute_time_diff_us(fechadura.efeito_pulso.inicio, get_absolute_time()) / 1000.0f;
                float brilho = (sinf(tempo_ms * (float)M_PI / 1500.0f) + 1.0f) / 2.0f;
                uint16_t r = (uint16_t)((float)fechadura.efeito_pulso.r * brilho * (PWM_MAX_DUTY / 255.0f));
                uint16_t g = (uint16_t)((float)fechadura.efeito_pulso.g * brilho * (PWM_MAX_DUTY / 255.0f));
                uint16_t b = (uint16_t)((float)fechadura.efeito_pulso.b * brilho * (PWM_MAX_DUTY / 255.0f));
                rgb_led_set_color(r, g, b);
                matriz_desenhar_ponto_central((uint8_t)(fechadura.efeito_pulso.r * brilho), (uint8_t)(fechadura.efeito_pulso.g * brilho), (uint8_t)(fechadura.efeito_pulso.b * brilho));
            
            // Animação para contagem de dígitos da senha.
            } else if (fechadura.modo_atual == MODO_AGUARDA_SENHA || fechadura.modo_atual == MODO_ADMIN_AGUARDANDO_NOVA_SENHA) {
                 matriz_desenhar_digitos(fechadura.digitos_count);
            
            // Animação de círculo de contagem regressiva para auto-travamento.
            } else if (fechadura.modo_atual == MODO_ABERTO) {
                 int64_t diff_us = absolute_time_diff_us(fechadura.timer_auto_trava.inicio, get_absolute_time());
                 int tempo_restante = TEMPO_AUTO_TRAVA_S - (diff_us / 1000000);
                 if (tempo_restante < 0) tempo_restante = 0;
                 matriz_animacao_circulo_tempo_update(tempo_restante);
            }
        }
        
        // --- Timers Gerais ---
        // Envia uma mensagem "heartbeat" para o broker MQTT a cada 30 segundos.
        if (timer_expirou(&fechadura.timer_heartbeat) || !fechadura.timer_heartbeat.ativo) {
            solicitar_publicacao_mqtt(MSG_LOG_HEARTBEAT, COR_NENHUMA);
            timer_iniciar(&fechadura.timer_heartbeat, 30000000);
        }
        // Desliga o servo motor após o tempo de movimento para economizar energia e evitar ruído.
        if (timer_expirou(&fechadura.timer_servo)) {
            servo_stop_move();
        }
        
        tight_loop_contents(); // Pequena pausa para o sistema operacional do Pico.
    }
    return 0; // Nunca deve chegar aqui.
}


/**
 * @brief Função principal do núcleo 1 (Core 1).
 * @details Responsável exclusivamente por gerenciar a conexão Wi-Fi e a comunicação MQTT.
 * Opera de forma independente do núcleo 0 para não bloquear a lógica principal.
 */
void funcao_wifi_nucleo1() {
    #define QUEUE_SIZE 10 // Tamanho da fila para mensagens MQTT a serem publicadas.
    
    // Fila circular para armazenar publicações MQTT.
    // Isso evita perder mensagens se o núcleo 0 as enviar mais rápido do que o núcleo 1 consegue publicar.
    typedef struct {
        char topico[100];
        char mensagem[100];
    } publication_t;
    static publication_t publication_queue[QUEUE_SIZE];
    static int queue_head = 0, queue_tail = 0;
    static TimerNaoBloqueante timer_entre_publicacoes; // Timer para espaçar as publicações MQTT.

    // --- Inicialização do Wi-Fi e MQTT ---
    cyw43_arch_init();
    cyw43_arch_enable_sta_mode();
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASS, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        multicore_fifo_push_blocking((FIFO_CMD_WIFI_CONECTADO << 16) | WIFI_STATUS_FAIL); // Avisa o Core 0 da falha.
    } else {
        multicore_fifo_push_blocking((FIFO_CMD_WIFI_CONECTADO << 16) | WIFI_STATUS_SUCCESS); // Avisa o Core 0 do sucesso.
    }
    iniciar_mqtt_cliente(); // Inicia o cliente MQTT (que tentará se conectar).

    // --- Loop Principal do Núcleo 1 ---
    while (true) {
        // Processa comandos e dados vindos do Core 0 via FIFO.
        if (multicore_fifo_rvalid()) {
            uint32_t pacote = multicore_fifo_pop_blocking();
            uint16_t comando = pacote >> 16;

            // Comando para publicar uma mensagem de log/status.
            if (comando == FIFO_CMD_PUBLICAR_MQTT) {
                uint16_t valor = pacote & 0xFFFF;
                uint8_t tipo_msg = valor & 0xFF;
                uint8_t cor_id = (valor >> 8) & 0xFF;
                char msg_buffer[100], cor_str[15], base_topic[100];
                
                // Mapeia o ID da cor para uma string.
                switch ((enum CorDetectada)cor_id) {
                    case COR_VERDE: strcpy(cor_str, "Verde"); break;
                    case COR_VERMELHA: strcpy(cor_str, "Vermelho"); break;
                    case COR_AZUL: strcpy(cor_str, "Azul"); break;
                    default: strcpy(cor_str, "N/A"); break;
                }
                
                // Constrói a mensagem e o tópico com base no tipo de mensagem recebido.
                switch ((enum MQTT_MSG_TYPE)tipo_msg) {
                    case MSG_STATUS_AGUARDANDO_CARTAO: strcpy(base_topic, TOPICO_STATUS); strcpy(msg_buffer, "Aguardando cartao"); break;
                    case MSG_STATUS_CARTAO_LIDO: strcpy(base_topic, TOPICO_STATUS); sprintf(msg_buffer, "Cartao %s lido", cor_str); break;
                    case MSG_STATUS_AGUARDANDO_SENHA: strcpy(base_topic, TOPICO_STATUS); strcpy(msg_buffer, "Aguardando senha"); break;
                    case MSG_STATUS_SISTEMA_ABERTO: strcpy(base_topic, TOPICO_STATUS); strcpy(msg_buffer, "Sistema Aberto"); break;
                    case MSG_STATUS_SISTEMA_FECHADO: strcpy(base_topic, TOPICO_STATUS); strcpy(msg_buffer, "Sistema Fechado"); break;
                    case MSG_STATUS_MODO_ADMIN: strcpy(base_topic, TOPICO_STATUS); strcpy(msg_buffer, "Modo Administracao"); break;
                    case MSG_LOG_ACESSO_OK: strcpy(base_topic, TOPICO_HISTORICO); sprintf(msg_buffer, "ACESSO LIBERADO: Cartao %s.", cor_str); break;
                    case MSG_LOG_ACESSO_FALHA: strcpy(base_topic, TOPICO_HISTORICO); sprintf(msg_buffer, "FALHA: Senha incorreta para o Cartao %s.", cor_str); break;
                    case MSG_LOG_EVENTO_TIMEOUT_SENHA: strcpy(base_topic, TOPICO_HISTORICO); strcpy(msg_buffer, "AVISO: Timeout para digitacao da senha."); break;
                    case MSG_LOG_EVENTO_AUTO_LOCK: strcpy(base_topic, TOPICO_HISTORICO); strcpy(msg_buffer, "EVENTO: Travamento automatico do sistema."); break;
                    case MSG_LOG_OPERACAO_CANCELADA: strcpy(base_topic, TOPICO_HISTORICO); strcpy(msg_buffer, "AVISO: Operacao cancelada pelo usuario."); break;
                    case MSG_LOG_ADMIN_INICIADO: strcpy(base_topic, TOPICO_HISTORICO); strcpy(msg_buffer, "ADMIN: Modo de alteracao de senha iniciado."); break;
                    case MSG_LOG_ADMIN_SENHA_ALTERADA: strcpy(base_topic, TOPICO_HISTORICO); sprintf(msg_buffer, "ADMIN: Senha para Cartao %s foi alterada.", cor_str); break;
                    case MSG_LOG_HEARTBEAT: strcpy(base_topic, TOPICO_HEARTBEAT); strcpy(msg_buffer, "ok"); break;
                    case MSG_ALARM_TEMP_ON: strcpy(base_topic, "alarme"); strcpy(msg_buffer, "{\"alarme\":\"temperatura\", \"status\":\"ativo\"}"); break;
                    case MSG_ALARM_TEMP_OFF: strcpy(base_topic, "alarme"); strcpy(msg_buffer, "{\"alarme\":\"temperatura\", \"status\":\"ok\"}"); break;
                }

                // Adiciona a publicação na fila.
                int next_tail = (queue_tail + 1) % QUEUE_SIZE;
                if (next_tail != queue_head) { // Verifica se a fila não está cheia.
                    snprintf(publication_queue[queue_tail].topico, sizeof(publication_queue[queue_tail].topico), "%s/%s", DEVICE_ID, base_topic);
                    strncpy(publication_queue[queue_tail].mensagem, msg_buffer, sizeof(publication_queue[queue_tail].mensagem) - 1);
                    publication_queue[queue_tail].mensagem[sizeof(publication_queue[queue_tail].mensagem) - 1] = '\0';
                    queue_tail = next_tail;
                }
            
            // Comando para publicar dados de sensores (temperatura ou umidade).
            } else if (comando == FIFO_CMD_PUB_SENSOR_TEMP || comando == FIFO_CMD_PUB_SENSOR_UMID) {
                uint16_t valor_int = pacote & 0xFFFF;
                char topico_final[100], msg_final[20];
                float valor_float = (float)valor_int / 100.0f;
                sprintf(msg_final, "%.2f", valor_float);

                if (comando == FIFO_CMD_PUB_SENSOR_TEMP) {
                    snprintf(topico_final, sizeof(topico_final), "%s/sensores/temperatura", DEVICE_ID);
                } else { // FIFO_CMD_PUB_SENSOR_UMID
                    snprintf(topico_final, sizeof(topico_final), "%s/sensores/umidade", DEVICE_ID);
                }
                
                // Adiciona a publicação de sensor na fila.
                int next_tail = (queue_tail + 1) % QUEUE_SIZE;
                if (next_tail != queue_head) { // Verifica se a fila não está cheia.
                    strncpy(publication_queue[queue_tail].topico, topico_final, sizeof(publication_queue[queue_tail].topico) - 1);
                    publication_queue[queue_tail].topico[sizeof(publication_queue[queue_tail].topico) - 1] = '\0';
                    strncpy(publication_queue[queue_tail].mensagem, msg_final, sizeof(publication_queue[queue_tail].mensagem) - 1);
                    publication_queue[queue_tail].mensagem[sizeof(publication_queue[queue_tail].mensagem) - 1] = '\0';
                    queue_tail = next_tail;
                }
            }
        }
        
        // Lógica para publicar mensagens da fila MQTT.
        if (!mqtt_is_publishing() && queue_head != queue_tail && (timer_expirou(&timer_entre_publicacoes) || !timer_entre_publicacoes.ativo)) {
            publication_t *pub = &publication_queue[queue_head];
            publicar_mensagem_mqtt(pub->topico, pub->mensagem);
            queue_head = (queue_head + 1) % QUEUE_SIZE; // Avança a cabeça da fila.
            timer_iniciar(&timer_entre_publicacoes, 50000); // Aguarda 50ms antes da próxima publicação.
        }
        
        cyw43_arch_poll(); // Função essencial para manter a pilha Wi-Fi/LwIP ativa.
        sleep_ms(1);       // Pequena pausa para economizar CPU.
    }
}