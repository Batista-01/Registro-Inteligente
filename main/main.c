#include <driver/i2c.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "HD44780.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "driver/timer.h"

//definição do fim de curso
#define INDIC_ON GPIO_NUM_19
#define INDIC_OFF GPIO_NUM_18
//Definições usadas para controle do motor de passos
#define IN1_GPIO GPIO_NUM_13
#define IN2_GPIO GPIO_NUM_12  // definição dos pinos usados para controle
#define IN3_GPIO GPIO_NUM_14
#define IN4_GPIO GPIO_NUM_27
#define STEP_DELAY 10         // Define o tempo de delay entre os passos do motor (quanto menor, maior a velocidade). Em milissegundos
// Sequência de passos de 4 fases para controle do motor


const int step_sequence[4][4] = {
    {1, 0, 0, 0},
    {0, 1, 0, 0},
    {0, 0, 1, 0},
    {0, 0, 0, 1},
};


// defines para LCD
#define LCD_ADDR 0x27
#define SDA_PIN  23
#define SCL_PIN  22
#define LCD_COLS 16
#define LCD_ROWS 2

// defines para interrupção de tempo
#define TIMER_BASE_CLK 80000000 //clock iterno de 80MHz
#define TIMER_DIVIDER         16  // Divisor para o timer de hardware (80 MHz / 16 = 5 MHz)
#define TIMER_SCALE          (TIMER_BASE_CLK/TIMER_DIVIDER)  // Fator de escala do timer
#define TIMER_INTERVAL_SEC    1.0  // Intervalo do timer (em segundos)


//variaveis de controle dos botões de interface com o usuario
volatile int bot1 ; // enter
volatile int bot2 ; // proximo/ aumenta
volatile int bot3 ; // diminui

// variaveis para controle de estado da tela
int tela_atual = 0;
int tela_anterior= 0;

// a variavel posição deve conter um valor entre 0 e 2048. ela representa a posição do registro
int posicao;

//variaveis para cauculo de vazão
volatile int pulsos_por_segundo;
volatile uint64_t contador ;

// funções utilizadas no sitema
void control_stepper_motor(int steps);
void exec_state_machine();
void run_state_machine();
void exibir_vazao_instantanea();
void set_vazao();
void set_position();
void reset_registro();
float caucular_vazao();


//funções para interrupção
static void IRAM_ATTR incremento_pulsos(void* arg);
void configuracoes_gpio_cont_pulsos();

static void IRAM_ATTR contagem_pulsos_por_segundo(void* arg);
void configuracoes_timer();

static void IRAM_ATTR set_estado_bot3(void* arg);
static void IRAM_ATTR set_estado_bot2(void* arg);
static void IRAM_ATTR set_estado_bot1(void* arg);
void configuracoes_gpio_botoes();






int app_main(){

    configuracoes_timer();
    configuracoes_gpio_cont_pulsos();
    configuracoes_gpio_botoes();
    LCD_init(LCD_ADDR, SDA_PIN, SCL_PIN, LCD_COLS, LCD_ROWS);
    reset_registro();

    while (true)
    {
    run_state_machine();  // monitora a tela que o usario quer estar, ou melhor, se o usuario quer sair da tela em que esta
    exec_state_machine(); //executa as ações correspondentes da tela
    }

}



//controle de estados do menu
void run_state_machine() {

    switch (tela_atual) {
        case 0:
            tela_atual=1;
            bot2=0;
            break;

        case 1:

            if (bot2 == true){
            tela_atual=2;
            bot2=0; 
            }
            break;

        case 2:
            if (bot2 == true){
            tela_atual=3;
            bot2=0;
            }
            break;

        case 3:
            if(bot2 == true) {
            tela_atual=1;
            bot2=0;
            }
            break;
        default:
            break;
    }
}



// controle de ações do menu
void exec_state_machine(){

    switch (tela_atual) {
        case 1: 
            if (tela_atual != tela_anterior){
                LCD_clearScreen();
                LCD_home();
                LCD_writeStr("> Vazão Atual:"); //caucula a vazão da um fluxo de agua
                LCD_setCursor(0, 1);
                tela_anterior =1; 
            }

            if (bot1 == true){
                exibir_vazao_instantanea();
                bot1=0;
            }
            break;

        case 2:
            if (tela_atual != tela_anterior){
                LCD_clearScreen();
                LCD_home();
                LCD_writeStr("> Setar Posição:");// seta a posição do desejada do registro
                tela_anterior =2; 
                } 

            if (bot1 == true){
                bot1=0;
                set_position();
            }
            break;
            
        case 3:
            if (tela_atual != tela_anterior){
                LCD_clearScreen();
                LCD_home();
                LCD_writeStr(">Setar Vazão:"); // setar vazão de desejada
                tela_anterior =3;
            }

            if (bot1 == true){
            bot1=0;
            set_vazao();
            tela_anterior =0; // esse comando é uma correção para que force o programa a entrar no primeiro if e imprimir novamente a tela doi no display

            }
            break;
        default:
            break;
    }
}



void control_stepper_motor(int steps) {
    // Configura os pinos GPIO como saídas

    esp_rom_gpio_pad_select_gpio(IN1_GPIO);
    gpio_set_direction(IN1_GPIO, GPIO_MODE_OUTPUT);

    esp_rom_gpio_pad_select_gpio(IN2_GPIO);
    gpio_set_direction(IN2_GPIO, GPIO_MODE_OUTPUT);

    esp_rom_gpio_pad_select_gpio(IN3_GPIO);
    gpio_set_direction(IN3_GPIO, GPIO_MODE_OUTPUT);

    esp_rom_gpio_pad_select_gpio(IN4_GPIO);
    gpio_set_direction(IN4_GPIO, GPIO_MODE_OUTPUT);

    // Variável para controlar o passo atual e direção
    int step = 0;
    int direction = steps > 0 ? 1 : -1;  // Define a direção (horário ou anti-horário)
    steps = abs(steps);  // Converte para positivo

    // Loop para movimentar o motor o número de passos desejado
    for (int i = 0; i < steps; i++) {
        // Define os valores dos pinos GPIO de acordo com a sequência de passos
        gpio_set_level(IN1_GPIO, step_sequence[step][0]);
        gpio_set_level(IN2_GPIO, step_sequence[step][1]);
        gpio_set_level(IN3_GPIO, step_sequence[step][2]);
        gpio_set_level(IN4_GPIO, step_sequence[step][3]);

        // Avança para o próximo passo na direção correta
        step = (step + direction + 4) % 4;

        // Atraso para controlar a velocidade do motor
        vTaskDelay(pdMS_TO_TICKS(STEP_DELAY));

    }
}

//caucula a vazão da um fluxo de agua
void exibir_vazao_instantanea(){  
    char str_vazao[20];
    int vazao= caucular_vazao();

    LCD_writeStr("Cauculando...");
    reset_registro();
    LCD_clearScreen(); LCD_home(); LCD_writeStr("> Vazão Atual:");
    sprintf(str_vazao, "%d", vazao);
    LCD_setCursor(6, 1); LCD_writeStr(str_vazao);
    LCD_setCursor(11,1); LCD_writeStr("L/H");
}

void set_position(){
    
    int posicao_desejada = 0;
    char posicao_str[20];

// recebendo entrada do usuario

    while (bot1 != true)
    {
        int acesso;

        if(bot2==true && posicao_desejada<100){
            bot2=0;
            posicao_desejada++;
            acesso++;
        }

        if (bot3==true && posicao_desejada>0){
            bot3=0;
            posicao_desejada--;
            acesso++;
        }

        if (acesso!=0){
        sprintf(posicao_str,"%d  ", posicao_desejada);
        LCD_setCursor(8,1);
        LCD_writeStr(posicao_str);
        }
        acesso=0;
    }
    
    bot1=0;
    //convertendo e cauculando paramentros a serem passados para o registro

    int passos = (posicao_desejada*2048/100) - posicao;  // posição final menos inicial
    posicao = posicao_desejada;
    control_stepper_motor(passos);
}


void set_vazao(){

    reset_registro();
    int vazao_maxima=caucular_vazao();

    int vazao_desejada = 0;
    char vazao_str[20];

// recebendo entrada do usuario

    while (bot1 != true)
    {
        int acesso;  // variavel que indica se houve modificação no valor inserido para que não seja necessario repetir a exibição no display

        if(bot2==true && vazao_desejada<(vazao_maxima-50)){
            bot2=0;
            vazao_desejada=vazao_desejada+50;
            acesso++;
            }

        if (bot3==true && vazao_desejada>50){
            bot3=0;
            vazao_desejada=vazao_desejada-50;
            acesso++;
        }

        if (acesso!=0){
            sprintf(vazao_str,"%d  ", vazao_desejada);
            LCD_setCursor(8,1);
            LCD_writeStr(vazao_str);
        }
        acesso=0;
    }

    LCD_clearScreen();
    LCD_home();
    LCD_writeStr("Regulando...");

    while (caucular_vazao() > vazao_desejada)
    {
        control_stepper_motor(8);
    }

    
}

void reset_registro(){
    
    esp_rom_gpio_pad_select_gpio(INDIC_OFF);
    gpio_set_direction(INDIC_OFF, GPIO_MODE_INPUT); // iniciando GPIOs dos fins de curso
    gpio_pulldown_en(INDIC_OFF);

    esp_rom_gpio_pad_select_gpio(INDIC_ON);
    gpio_set_direction(INDIC_ON, GPIO_MODE_INPUT);
    gpio_pulldown_en(INDIC_ON);

    while ( gpio_get_level(INDIC_ON) != true)
    {
        control_stepper_motor(-8);
    }
    posicao=0;
}


float caucular_vazao(){
int num_pulsos_atuais;
num_pulsos_atuais = pulsos_por_segundo;
float vazao;
vazao= (3600.0*num_pulsos_atuais)/450.0;
return vazao;
}



// ========================interrupção GPIO Botões ========================

static void IRAM_ATTR set_estado_bot1(void* arg){
    if(bot1 == 0)
    bot1=1;
}
static void IRAM_ATTR set_estado_bot2(void* arg){
    if(bot2 == 0)
    bot2=1;
}
static void IRAM_ATTR set_estado_bot3(void* arg){
    if(bot3 == 0)
    bot3=1;
}

void configuracoes_gpio_botoes(){
    gpio_config_t io_conf;
        io_conf.intr_type = GPIO_INTR_NEGEDGE;         // Configura para interrupção na borda de subida
        io_conf.mode = GPIO_MODE_INPUT;                // Configura o pino como entrada
        io_conf.pin_bit_mask = ((1ULL << GPIO_NUM_4) | (1ULL << GPIO_NUM_16) | (1ULL << GPIO_NUM_17) );  
        io_conf.pull_down_en = 1;                      // Habilita o pull-down
        io_conf.pull_up_en = 0;                        
        gpio_config(&io_conf);                         // Aplica a configuração

    gpio_isr_handler_add(GPIO_NUM_4, set_estado_bot1, NULL);
    gpio_isr_handler_add(GPIO_NUM_16, set_estado_bot2, NULL);
    gpio_isr_handler_add(GPIO_NUM_17, set_estado_bot3, NULL);

}


//-----------------interrupção de GPIO contagem de pulsos --------------
static void IRAM_ATTR incremento_pulsos(void* arg){
  contador++;
}

void configuracoes_gpio_cont_pulsos(){
  gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_POSEDGE;         // Configura para interrupção na borda de subida
    io_conf.mode = GPIO_MODE_INPUT;                // Configura o pino como entrada
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_34);  // Seleciona o pino 32
    io_conf.pull_down_en = 0;                      // Desabilita o pull-down
    io_conf.pull_up_en = 0;                        // Habilita o pull-up
    gpio_config(&io_conf);                         // Aplica a configuração

gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1); // ativa os serviços de interrupção em gpio e passando tag de preferência de prioridade das interrupções
gpio_isr_handler_add(GPIO_NUM_34, incremento_pulsos, NULL); /* define a função que a interrupção no pino 32 irá chamar (IRS)*/
}



// ------------ interrrupção de tempo e configurações relacionadas ---------------
static void IRAM_ATTR contagem_pulsos_por_segundo(void* arg){
  pulsos_por_segundo = contador;
  contador = 0;

  timer_group_clr_intr_status_in_isr(TIMER_GROUP_0, TIMER_0);  // Limpa a interrupção do timer
  timer_group_enable_alarm_in_isr(TIMER_GROUP_0, TIMER_0); // essa função reinicia o alarme. Aparentemente o auto reload não esta funcionando
}

void configuracoes_timer(){
    timer_config_t timer_conf = {    // essa foi a unica forma de configurar a estrutura que fez o codigo funcionar corretamente. Setando as conf uma a uma não deu certo
        .divider = TIMER_DIVIDER,
        .counter_dir = TIMER_COUNT_UP,
        .counter_en = TIMER_PAUSE,
        .alarm_en = TIMER_ALARM_EN,
        .auto_reload = true
    };

    timer_init(TIMER_GROUP_0, TIMER_0, &timer_conf); // Inicializa o timer do grupo 0, timer 0
    timer_set_alarm_value(TIMER_GROUP_0, TIMER_0, TIMER_INTERVAL_SEC * TIMER_SCALE); // Define o valor do alarme (1 segundo convertido para ticks)
    timer_enable_intr(TIMER_GROUP_0, TIMER_0);// Habilita a interrupção do alarme
    timer_isr_register(TIMER_GROUP_0, TIMER_0, contagem_pulsos_por_segundo, NULL, ESP_INTR_FLAG_IRAM, NULL);// Vincula a função de interrupção ao timer
    timer_start(TIMER_GROUP_0, TIMER_0);// Inicia o timer
}