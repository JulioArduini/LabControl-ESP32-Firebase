// OCORRENCIAS INFORMADAS NA DEVOLUCAO - ESP32 + RC522 + Firebase
// Configure as credenciais abaixo antes de enviar ao ESP32.
// ATENCAO: use uma conta de testes e regras de acesso restritas no Firebase.
// Esta versao reduz falhas, mas NAO oferece transacao atomica entre caminhos.

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <time.h>

// =====================================================
// WIFI
// =====================================================

#include "config.h"

// =====================================================
// FIREBASE
// =====================================================


UserAuth user_auth(API_KEY, USER_EMAIL, USER_PASSWORD);

FirebaseApp app;
WiFiClientSecure ssl_client;

using AsyncClient = AsyncClientClass;
AsyncClient async_client(ssl_client);

RealtimeDatabase Database;

// =====================================================
// RFID RC522
// =====================================================

#define SS_PIN 5
#define RST_PIN 22
#define BOTAO_SIM 26
#define BOTAO_NAO 27

MFRC522 rfid(SS_PIN, RST_PIN);

// =====================================================
// DADOS DO ALUNO
// =====================================================

String nomeAluno = "";
String matriculaAluno = "";
String rfidAluno = "";

bool aguardandoEquipamento = false; // utilizado pelas funcoes de historico
enum Estado { INICIO, ESCOLHA, AGUARDANDO_ALUNO, DEVOLUCAO, PERGUNTA_PROBLEMA, SELECIONA_PROBLEMA };
Estado estado = INICIO;
String equipamentoDevolucao = "";
int opcaoProblema = 0;
const char* TIPOS_PROBLEMA[] = {"NAO_LIGA", "DANIFICADO", "ACESSORIO_FALTANDO", "OUTRO"};
const char* DESCRICOES_PROBLEMA[] = {"Equipamento nao liga", "Equipamento danificado", "Acessorio faltando", "Outro problema (detalhar com responsavel)"};
String produtoSelecionado = "";
String uidProdutoSelecionado = "";
unsigned long inicioAviso = 0;
bool avisoIndisponivel = false;
unsigned long ultimoBotao = 0;
unsigned long inicioEtapa = 0;
const unsigned long TEMPO_LIMITE = 60000;

void voltarInicio() {
  estado = INICIO;
  equipamentoDevolucao = "";
  opcaoProblema = 0;
  produtoSelecionado = "";
  uidProdutoSelecionado = "";
  nomeAluno = "";
  matriculaAluno = "";
  rfidAluno = "";
  aguardandoEquipamento = false;
  inicioEtapa = 0;
  Serial.println("\nAproxime o RFID de um equipamento para consultar.");
  Serial.println("Ou aproxime o cartao do aluno para iniciar devolucao.");
}

// =====================================================
// DATA E HORA (HORARIO DE MANAUS: UTC-4)
// =====================================================

String obterDataHora() {
  struct tm info;

  if (!getLocalTime(&info, 10000)) {
    Serial.println("Aviso: relogio ainda nao sincronizado com NTP.");
    return "data_indisponivel";
  }

  char buffer[25];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &info);
  return String(buffer);
}

String buscarNomeEquipamento(String equipamento);

// =====================================================
// OCORRENCIAS INFORMADAS NA DEVOLUCAO
// Nao modifica emprestimos, devolucoes nem status.
// =====================================================

bool gravarCampoOcorrencia(const String &caminho, const String &valor) {
  Database.set<String>(async_client, caminho, valor);
  if (async_client.lastError().code() != 0) {
    Serial.print("Falha ao gravar ocorrencia: ");
    Serial.println(async_client.lastError().message());
    return false;
  }
  return true;
}

bool registrarOcorrenciaDevolucao(const String &tipo,
                                  const String &descricao,
                                  const String &equipamento) {
  if (!app.ready() || WiFi.status() != WL_CONNECTED) {
    Serial.println("Ocorrencia nao enviada: sem conexao Firebase.");
    return false;
  }

  // Epoch + contador de boot + numero aleatorio reduzem colisoes de IDs.
  time_t agora = time(nullptr);
  String id = String((unsigned long)agora) + "_" +
              String(millis()) + "_" + String((unsigned long)esp_random(), HEX);
  String base = "/ocorrencias/devolucao_" + id;
  String data = obterDataHora();

  // Grava primeiro os campos e publica o status por ultimo.
  // Uma falha pode deixar uma ocorrencia parcial; nunca altera o equipamento.
  if (!gravarCampoOcorrencia(base + "/origem", "INFORMADA_NA_DEVOLUCAO")) return false;
  if (!gravarCampoOcorrencia(base + "/tipo", tipo)) return false;
  if (!gravarCampoOcorrencia(base + "/descricao", descricao)) return false;
  if (!gravarCampoOcorrencia(base + "/equipamento", equipamento)) return false;
  String nomeEquipamento = buscarNomeEquipamento(equipamento);
  if (async_client.lastError().code() != 0 || nomeEquipamento.length() == 0) return false;
  if (!gravarCampoOcorrencia(base + "/nomeEquipamento", nomeEquipamento)) return false;
  if (!gravarCampoOcorrencia(base + "/Nome", nomeAluno)) return false;
  if (!gravarCampoOcorrencia(base + "/Matricula", matriculaAluno)) return false;
  if (!gravarCampoOcorrencia(base + "/dataOcorrencia", data)) return false;
  if (!gravarCampoOcorrencia(base + "/status", "aberta")) return false;

  Serial.println("Ocorrencia registrada: " + base);
  return true;
}

// =====================================================
// FORMATAR UID DO CARTAO
// =====================================================

String formatarUID(MFRC522::Uid *uid) {
  String resultado = "";

  for (byte i = 0; i < uid->size; i++) {
    if (uid->uidByte[i] < 0x10) {
      resultado += "0";
    }

    resultado += String(uid->uidByte[i], HEX);

    if (i < uid->size - 1) {
      resultado += ":";
    }
  }

  resultado.toUpperCase();
  return resultado;
}

// =====================================================
// BUSCAR ALUNO PELO RFID
// =====================================================

bool buscarUsuario(String uid) {
  Serial.println();

  uid.trim();
  uid.toUpperCase();

  String dados = Database.get<String>(
    async_client,
    "/Usuario"
  );

  if (async_client.lastError().code() != 0) {
    Serial.println("Erro ao consultar usuarios.");
    Serial.println(async_client.lastError().message());
    return false;
  }


  String procura = "\"rfid\":\"" + uid + "\"";
  int posRFID = dados.indexOf(procura);

  if (posRFID == -1) {
    procura = "\"rfid\": \"" + uid + "\"";
    posRFID = dados.indexOf(procura);
  }

  if (posRFID == -1) {
    Serial.println("Usuario nao encontrado.");
    return false;
  }

  int inicioObjeto = dados.lastIndexOf("{", posRFID);

  if (inicioObjeto == -1) {
    Serial.println("Objeto do usuario nao encontrado.");
    return false;
  }

  // Buscar nome
  int posNome = dados.indexOf("\"Nome\"", inicioObjeto);

  if (posNome == -1) {
    Serial.println("Nome nao encontrado.");
    return false;
  }

  int doisPontosNome = dados.indexOf(":", posNome);
  int inicioNome = dados.indexOf("\"", doisPontosNome + 1);
  int fimNome = dados.indexOf("\"", inicioNome + 1);

  if (doisPontosNome == -1 ||
      inicioNome == -1 ||
      fimNome == -1) {
    Serial.println("Formato do nome invalido.");
    return false;
  }

  nomeAluno = dados.substring(inicioNome + 1, fimNome);

  // Buscar matricula
  int posMatricula = dados.indexOf(
    "\"Matricula\"",
    inicioObjeto
  );

  if (posMatricula == -1) {
    Serial.println("Matricula nao encontrada.");
    return false;
  }

  int doisPontosMatricula = dados.indexOf(":", posMatricula);
  int inicioMatricula = doisPontosMatricula + 1;

  while (inicioMatricula < dados.length() &&
         (dados[inicioMatricula] == ' ' ||
          dados[inicioMatricula] == '"')) {
    inicioMatricula++;
  }

  int fimMatricula = inicioMatricula;

  while (fimMatricula < dados.length() &&
         dados[fimMatricula] != ',' &&
         dados[fimMatricula] != '}' &&
         dados[fimMatricula] != '"') {
    fimMatricula++;
  }

  matriculaAluno = dados.substring(
    inicioMatricula,
    fimMatricula
  );

  matriculaAluno.trim();
  rfidAluno = uid;

  // Verificar se o cadastro esta ativo
  int posStatus = dados.indexOf("\"Status\"", inicioObjeto);

  if (posStatus != -1) {
    int doisPontosStatus = dados.indexOf(":", posStatus);
    String statusTexto = dados.substring(
      doisPontosStatus + 1,
      doisPontosStatus + 10
    );

    statusTexto.trim();

    if (!statusTexto.startsWith("true")) {
      Serial.println("Usuario inativo ou sem permissao.");
      return false;
    }
  }

  Serial.println();
  Serial.println("--------------------------------");
  Serial.println("USUARIO IDENTIFICADO");
  Serial.println("================================");
  Serial.print("Nome: ");
  Serial.println(nomeAluno);
  Serial.print("Matricula: ");
  Serial.println(matriculaAluno);
  Serial.println("================================");

  return true;
}

// =====================================================
// IDENTIFICAR EQUIPAMENTO PELO RFID
// =====================================================

String identificarEquipamento(String uid) {
  Serial.println();

  uid.trim();
  uid.toUpperCase();

  String dados = Database.get<String>(
    async_client,
    "/Equipamento"
  );

  if (async_client.lastError().code() != 0) {
    Serial.println("Erro ao consultar equipamentos.");
    Serial.println(async_client.lastError().message());
    return "";
  }


  String procura = "\"rfid\":\"" + uid + "\"";
  int posRFID = dados.indexOf(procura);

  if (posRFID == -1) {
    procura = "\"rfid\": \"" + uid + "\"";
    posRFID = dados.indexOf(procura);
  }

  if (posRFID == -1) {
    Serial.println("Equipamento nao encontrado.");
        return "";
  }

  // Localizar o objeto que contem o RFID
  int inicioObjeto = dados.lastIndexOf("{", posRFID);

  if (inicioObjeto == -1) {
    Serial.println("Objeto do equipamento nao encontrado.");
    return "";
  }

  // Encontrar a chave que identifica o equipamento
  int doisPontos = dados.lastIndexOf(':', inicioObjeto);

  if (doisPontos == -1) {
    Serial.println("Chave do equipamento nao encontrada.");
    return "";
  }

  int fimAspas = dados.lastIndexOf('"', doisPontos - 1);

  if (fimAspas == -1) {
    return "";
  }

  int inicioAspas = dados.lastIndexOf('"', fimAspas - 1);

  if (inicioAspas == -1) {
    return "";
  }

  String equipamento = dados.substring(
    inicioAspas + 1,
    fimAspas
  );

  return equipamento;
}

// =====================================================
// BUSCAR NOME DO EQUIPAMENTO
// =====================================================

String buscarNomeEquipamento(String equipamento) {
  String caminho =
    "/Equipamento/" + equipamento + "/Nome";

  String nome = Database.get<String>(
    async_client,
    caminho
  );

  if (async_client.lastError().code() != 0) {
    Serial.println("Erro ao buscar nome do equipamento.");
    Serial.println(async_client.lastError().message());
    return "";
  }

  nome.trim();
  return nome;
}

// =====================================================
// CONSULTAR STATUS DO EQUIPAMENTO
// true = disponivel
// false = emprestado
// =====================================================

bool pegarStatusEquipamento(String equipamento) {
  String caminho =
    "/Equipamento/" + equipamento + "/status";

  Serial.println();

  bool status = Database.get<bool>(
    async_client,
    caminho
  );

  if (async_client.lastError().code() != 0) {
    Serial.println("Erro ao consultar status.");
    Serial.println(async_client.lastError().message());
    return false;
  }



  return status;
}

// =====================================================
// CONSULTAR USUARIO ATUAL DO EQUIPAMENTO
// =====================================================

String pegarUsuarioEquipamento(String equipamento) {
  String caminho =
    "/Equipamento/" + equipamento + "/usuario";

  String usuario = Database.get<String>(
    async_client,
    caminho
  );

  if (async_client.lastError().code() != 0) {
    Serial.println("Erro ao consultar usuario atual.");
    return "";
  }

  usuario.trim();
  return usuario;
}

// =====================================================
// CONSULTAR MATRICULA ATUAL
// =====================================================

String pegarMatriculaEquipamento(String equipamento) {
  String caminho =
    "/Equipamento/" + equipamento + "/matricula";

  String matricula = Database.get<String>(
    async_client,
    caminho
  );

  if (async_client.lastError().code() != 0) {
    Serial.println("Erro ao consultar matricula atual.");
    return "";
  }

  matricula.trim();
  return matricula;
}

// =====================================================
// REGISTRAR EMPRESTIMO
// Inclui nomeEquipamento no historico
// =====================================================

// Cada gravacao e conferida antes de prosseguir.
// IMPORTANTE: varias escritas separadas nao formam transacao atomica.
bool gravarTexto(String caminho, String valor) {
  Database.set<String>(async_client, caminho, valor);
  if (async_client.lastError().code() != 0) {
    Serial.print("Falha ao gravar: "); Serial.println(caminho);
    Serial.println(async_client.lastError().message());
    return false;
  }
  return true;
}

bool gravarBooleano(String caminho, bool valor) {
  Database.set<bool>(async_client, caminho, valor);
  if (async_client.lastError().code() != 0) {
    Serial.print("Falha ao gravar: "); Serial.println(caminho);
    Serial.println(async_client.lastError().message());
    return false;
  }
  return true;
}

void registrarEmprestimo(String equipamento, String uidEquipamento) {
  Serial.println("\n================================");
  Serial.println("REGISTRANDO EMPRESTIMO");
  Serial.println("================================");

  String nomeEquipamento = buscarNomeEquipamento(equipamento);
  if (async_client.lastError().code() != 0 || nomeEquipamento.length() == 0) {
    Serial.println("Nome do equipamento indisponivel. Cancelado.");
    return;
  }
  String data = obterDataHora();
  if (data == "data_indisponivel") {
    Serial.println("Horario nao sincronizado. Emprestimo cancelado.");
    return;
  }

  // Timestamp + numero aleatorio: nao depende apenas de millis().
  time_t agora = time(nullptr);
  String idEmprestimo = String((unsigned long)agora) + "_" + String(esp_random(), HEX);
  String historico = "/emprestimos/" + idEmprestimo;
  String base = "/Equipamento/" + equipamento;

  // Primeiro registra o historico. Se falhar, nao bloqueia o equipamento.
  if (!gravarTexto(historico + "/dataEmprestimo", data) ||
      !gravarTexto(historico + "/Nome", nomeAluno) ||
      !gravarTexto(historico + "/Matricula", matriculaAluno) ||
      !gravarTexto(historico + "/rfidUsuario", rfidAluno) ||
      !gravarTexto(historico + "/equipamento", equipamento) ||
      !gravarTexto(historico + "/nomeEquipamento", nomeEquipamento) ||
      !gravarTexto(historico + "/rfidEquipamento", uidEquipamento) ||
      !gravarTexto(historico + "/status", "preparando")) {
    Serial.println("Historico incompleto; equipamento continua disponivel.");
    return;
  }

  if (!gravarTexto(base + "/usuario", nomeAluno) ||
      !gravarTexto(base + "/matricula", matriculaAluno) ||
      !gravarTexto(base + "/idEmprestimo", idEmprestimo)) {
    Serial.println("Falha ao associar aluno. Verifique dados antes de tentar novamente.");
    return;
  }

  // O status do equipamento so muda apos os dados essenciais serem salvos.
  if (!gravarBooleano(base + "/status", false)) {
    Serial.println("Falha ao marcar equipamento como emprestado.");
    return;
  }

  if (!gravarTexto(historico + "/status", "emprestado")) {
    Serial.println("ATENCAO: equipamento emprestado, mas historico ficou pendente!");
    Serial.println("Corrija no Firebase antes de novo emprestimo.");
    return;
  }

  Serial.println("EMPRESTIMO REGISTRADO!");
  Serial.println("Aluno: " + nomeAluno);
  Serial.println("Matricula: " + matriculaAluno);
  Serial.println("Equipamento: " + nomeEquipamento);
  Serial.println("================================");
}

// =====================================================
// REGISTRAR DEVOLUCAO
// =====================================================

void registrarDevolucao(String equipamento) {
  Serial.println();
  Serial.println("================================");
  Serial.println("REGISTRANDO DEVOLUCAO");
  Serial.println("================================");

  String caminhoEquipamento = "/Equipamento/" + equipamento;

  // Recuperar o ID do historico do emprestimo ativo
  String idEmprestimo = Database.get<String>(
    async_client,
    caminhoEquipamento + "/idEmprestimo"
  );

  if (async_client.lastError().code() != 0) {
    Serial.println("Erro ao consultar ID do emprestimo.");
    Serial.println(async_client.lastError().message());
    return;
  }

  idEmprestimo.trim();
  idEmprestimo.replace("\"", "");

  if (idEmprestimo.length() == 0 || idEmprestimo == "null") {
    Serial.println("Nao foi encontrado ID do historico para este equipamento.");
    Serial.println("Este emprestimo pode ter sido criado antes da atualizacao do codigo.");
    Serial.println("Devolucao cancelada para evitar perder o historico.");
    return;
  }

  String caminhoHistorico = "/emprestimos/" + idEmprestimo;
  String caminhoDevolucao = "/devolucoes/" + idEmprestimo;
  String dataDevolucao = obterDataHora();
  if (dataDevolucao == "data_indisponivel") {
    Serial.println("Relogio nao sincronizado. Devolucao cancelada.");
    return;
  }

  // Cria registro separado de devolucao, usando o mesmo ID do emprestimo.
  // Se houver erro, nao libera o equipamento nem marca como devolvido.
  String nomeEquipamento = buscarNomeEquipamento(equipamento);
  if (async_client.lastError().code() != 0 || nomeEquipamento.length() == 0) {
    Serial.println("Erro ao consultar nome do equipamento.");
    return;
  }

  Database.set<String>(async_client, caminhoDevolucao + "/idEmprestimo", idEmprestimo);
  if (async_client.lastError().code() != 0) { Serial.println("Erro ao salvar ID da devolucao."); return; }
  Database.set<String>(async_client, caminhoDevolucao + "/equipamento", equipamento);
  if (async_client.lastError().code() != 0) { Serial.println("Erro ao salvar equipamento na devolucao."); return; }
  Database.set<String>(async_client, caminhoDevolucao + "/nomeEquipamento", nomeEquipamento);
  if (async_client.lastError().code() != 0) { Serial.println("Erro ao salvar nome do equipamento."); return; }
  Database.set<String>(async_client, caminhoDevolucao + "/Nome", nomeAluno);
  if (async_client.lastError().code() != 0) { Serial.println("Erro ao salvar nome do aluno."); return; }
  Database.set<String>(async_client, caminhoDevolucao + "/Matricula", matriculaAluno);
  if (async_client.lastError().code() != 0) { Serial.println("Erro ao salvar matricula do aluno."); return; }
  Database.set<String>(async_client, caminhoDevolucao + "/dataDevolucao", dataDevolucao);
  if (async_client.lastError().code() != 0) { Serial.println("Erro ao salvar data na devolucao."); return; }

  // Mantem o historico antigo atualizado.
  Database.set<String>(async_client, caminhoHistorico + "/dataDevolucao", dataDevolucao);
  if (async_client.lastError().code() != 0) {
    Serial.println("Erro ao salvar data no historico. Devolucao pendente.");
    return;
  }
  Database.set<String>(async_client, caminhoHistorico + "/status", "devolvido");
  if (async_client.lastError().code() != 0) {
    Serial.println("Erro ao atualizar historico. Devolucao pendente.");
    return;
  }

  // Liberar equipamento
  Database.set<bool>(
    async_client,
    caminhoEquipamento + "/status",
    true
  );

  if (async_client.lastError().code() != 0) {
    Serial.println("Erro ao liberar equipamento.");
    Serial.println(async_client.lastError().message());
    return;
  }

  // Limpar o nome do responsavel
  Database.set<String>(
    async_client,
    caminhoEquipamento + "/usuario",
    ""
  );

  if (async_client.lastError().code() != 0) {
    Serial.println("Erro ao limpar usuario do equipamento.");
    Serial.println(async_client.lastError().message());
    return;
  }

  // Limpar a matricula do responsavel
  Database.set<String>(
    async_client,
    caminhoEquipamento + "/matricula",
    ""
  );

  if (async_client.lastError().code() != 0) {
    Serial.println("Erro ao limpar matricula do equipamento.");
    Serial.println(async_client.lastError().message());
    return;
  }

  // Limpar o ID ativo do equipamento. O ID continua no caminho do historico.
  Database.set<String>(
    async_client,
    caminhoEquipamento + "/idEmprestimo",
    ""
  );

  if (async_client.lastError().code() != 0) {
    Serial.println("Aviso: devolucao registrada, mas nao foi possivel limpar idEmprestimo.");
    Serial.println(async_client.lastError().message());
  }

  Serial.println();
  Serial.println("DEVOLUCAO REGISTRADA!");
  Serial.print("Nome do material: ");
  Serial.println(nomeEquipamento);
  Serial.print("Equipamento: ");
  Serial.println(equipamento);
  Serial.println("Status: DISPONIVEL");
  Serial.println("================================");

  aguardandoEquipamento = false;
  nomeAluno = "";
  matriculaAluno = "";
  rfidAluno = "";
}

// =====================================================
// CONSULTA DO EQUIPAMENTO (SEM ALTERAR FIREBASE)
// =====================================================
void consultarProduto(String equipamento, String uid) {
  String base = "/Equipamento/" + equipamento;
  String nome = buscarNomeEquipamento(equipamento);
  if (async_client.lastError().code() != 0 || nome.length() == 0) {
    Serial.println("Falha ao consultar nome. Operacao cancelada.");
    voltarInicio();
    return;
  }

  String compartimento = Database.get<String>(async_client, base + "/Compartimento");
  if (async_client.lastError().code() != 0) {
    Serial.println("Falha ao consultar compartimento.");
    voltarInicio();
    return;
  }
  compartimento.replace("\"", "");
  compartimento.trim();

  bool disponivel = pegarStatusEquipamento(equipamento);
  if (async_client.lastError().code() != 0) {
    Serial.println("Falha ao verificar disponibilidade. Nenhum emprestimo sera feito.");
    voltarInicio();
    return;
  }

  Serial.println("\n================================");
  Serial.println("EQUIPAMENTO IDENTIFICADO");
  Serial.println("================================");
  Serial.println("Nome: " + nome);
  Serial.println("ID: " + equipamento);
  Serial.println("Compartimento: " + compartimento);
  Serial.println(disponivel ? "Status: DISPONIVEL" : "Status: EMPRESTADO");
  Serial.println("================================");

  if (!disponivel) {
    Serial.println("Indisponivel. Retornando em 5 segundos...");
    avisoIndisponivel = true;
    inicioAviso = millis();
    return;
  }

  produtoSelecionado = equipamento;
  uidProdutoSelecionado = uid;
  estado = ESCOLHA;
  inicioEtapa = millis();
  Serial.println("Deseja realizar o emprestimo?");
  Serial.println("SIM (GPIO 26) | NAO (GPIO 27)");
}

void tentarDevolucao(String equipamento) {
  bool disponivel = pegarStatusEquipamento(equipamento);
  if (async_client.lastError().code() != 0) {
    Serial.println("Erro ao consultar equipamento.");
    voltarInicio();
    return;
  }
  if (disponivel) {
    Serial.println("Equipamento ja esta disponivel. Nao ha devolucao pendente.");
    voltarInicio();
    return;
  }
  String usuario = pegarUsuarioEquipamento(equipamento);
  if (async_client.lastError().code() != 0) {
    voltarInicio();
    return;
  }
  String matricula = pegarMatriculaEquipamento(equipamento);
  if (async_client.lastError().code() != 0) {
    voltarInicio();
    return;
  }
  if (usuario == nomeAluno && matricula == matriculaAluno &&
      usuario.length() > 0 && matricula.length() > 0) {
    equipamentoDevolucao = equipamento;
    estado = PERGUNTA_PROBLEMA;
    inicioEtapa = millis();
    Serial.println("\nO equipamento apresenta algum problema?");
    Serial.println("SIM = informar problema | NAO = devolver normalmente");
  } else {
    Serial.println("Devolucao negada: equipamento nao pertence ao aluno.");
  }
  if (estado == DEVOLUCAO) voltarInicio();
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(9600);
  pinMode(BOTAO_SIM, INPUT_PULLUP);
  pinMode(BOTAO_NAO, INPUT_PULLUP);
  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println("LABCONTROL");
  Serial.println("================================");

  // Inicializar RFID
  SPI.begin();
  rfid.PCD_Init();

  Serial.println("RFID pronto.");

  // Conectar Wi-Fi
  Serial.println();
  Serial.println("Conectando ao WiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  Serial.println();
  Serial.println("WiFi conectado!");

  // Sincronizar data/hora pela internet (Manaus: UTC-4)
  configTime(-4 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  // Inicializar Firebase
  ssl_client.setInsecure();

  Serial.println();
  Serial.println("Conectando ao Firebase...");

  initializeApp(
    async_client,
    app,
    getAuth(user_auth),
    120000
  );

  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);

  // Aguardar autenticacao
  Serial.println();

  unsigned long inicio = millis();

  while (!app.ready() && millis() - inicio < 120000) {
    app.loop();
    delay(100);
  }

  Serial.println();

  if (!app.ready()) {
    Serial.println("Falha na autenticacao do Firebase.");
    Serial.println("Verifique email, senha e API_KEY.");
    return;
  }

  Serial.println("Firebase autenticado!");

  Serial.println();
  Serial.println("================================");
  Serial.println("SISTEMA PRONTO!");
  Serial.println("================================");
  voltarInicio();
  Serial.println("================================");
}

// =====================================================
// LOOP PRINCIPAL
// =====================================================
void loop() {
  app.loop();
  if (!app.ready()) {
    static unsigned long ultimoAviso = 0;
    if (millis() - ultimoAviso > 5000) {
      Serial.println("Aguardando conexao/autenticacao Firebase...");
      ultimoAviso = millis();
    }
    return;
  }

  if (avisoIndisponivel) {
    if (millis() - inicioAviso >= 5000) {
      avisoIndisponivel = false;
      voltarInicio();
    }
    return;
  }

  if (estado != INICIO && inicioEtapa != 0 && millis() - inicioEtapa >= TEMPO_LIMITE) {
    Serial.println("Tempo esgotado. Voltando para consulta inicial.");
    voltarInicio();
    return;
  }

  if ((estado == AGUARDANDO_ALUNO || estado == DEVOLUCAO) && digitalRead(BOTAO_NAO) == LOW && millis() - ultimoBotao > 250) {
    ultimoBotao = millis();
    Serial.println("Operacao cancelada pelo botao NAO.");
    voltarInicio();
    return;
  }

  // Pergunta sobre defeitos SOMENTE apos identificar aluno e equipamento na devolucao.
  if (estado == PERGUNTA_PROBLEMA && millis() - ultimoBotao > 350) {
    if (digitalRead(BOTAO_NAO) == LOW) {
      ultimoBotao = millis();
      Serial.println("Sem problema informado. Finalizando devolucao...");
      registrarDevolucao(equipamentoDevolucao);
      voltarInicio();
      return;
    }
    if (digitalRead(BOTAO_SIM) == LOW) {
      ultimoBotao = millis();
      opcaoProblema = 0;
      estado = SELECIONA_PROBLEMA;
      inicioEtapa = millis();
      Serial.println("\nSelecione o tipo de problema:");
      Serial.println("SIM = proxima opcao | NAO = confirmar opcao");
      Serial.println(String(opcaoProblema + 1) + " - " + DESCRICOES_PROBLEMA[opcaoProblema]);
      return;
    }
  }

  if (estado == SELECIONA_PROBLEMA && millis() - ultimoBotao > 350) {
    if (digitalRead(BOTAO_SIM) == LOW) {
      ultimoBotao = millis();
      opcaoProblema = (opcaoProblema + 1) % 4;
      inicioEtapa = millis();
      Serial.println(String(opcaoProblema + 1) + " - " + DESCRICOES_PROBLEMA[opcaoProblema]);
      return;
    }
    if (digitalRead(BOTAO_NAO) == LOW) {
      ultimoBotao = millis();
      Serial.println("Salvando ocorrencia antes da devolucao...");
      bool salva = registrarOcorrenciaDevolucao(
        TIPOS_PROBLEMA[opcaoProblema],
        DESCRICOES_PROBLEMA[opcaoProblema],
        equipamentoDevolucao
      );
      if (!salva) {
        Serial.println("FALHA: ocorrencia nao confirmada. Devolucao NAO finalizada.");
        Serial.println("Confira a conexao e pressione NAO para tentar novamente.");
        inicioEtapa = millis();
        return;
      }
      Serial.println("Ocorrencia salva. Finalizando devolucao...");
      registrarDevolucao(equipamentoDevolucao);
      voltarInicio();
      return;
    }
  }

  // Os botoes so atuam quando existe produto disponivel consultado.
  if (estado == ESCOLHA && millis() - ultimoBotao > 250) {
    if (digitalRead(BOTAO_NAO) == LOW) {
      ultimoBotao = millis();
      Serial.println("Consulta cancelada. Nenhum emprestimo registrado.");
      voltarInicio();
      return;
    }
    if (digitalRead(BOTAO_SIM) == LOW) {
      ultimoBotao = millis();
      estado = AGUARDANDO_ALUNO;
      inicioEtapa = millis();
      Serial.println("\nSIM selecionado. Aproxime o cartao RFID do aluno.");
      return;
    }
  }

  // Ignorar cartoes enquanto espera SIM/NAO.
  if (estado == ESCOLHA || estado == PERGUNTA_PROBLEMA || estado == SELECIONA_PROBLEMA) return;

  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  String uid = formatarUID(&rfid.uid);
  Serial.println("\nRFID detectado: " + uid);

  if (estado == INICIO) {
    // Primeiro tenta reconhecer equipamento; se nao existir, tenta aluno.
    String equipamento = identificarEquipamento(uid);
    if (equipamento.length() > 0) {
      consultarProduto(equipamento, uid);
    } else if (async_client.lastError().code() == 0 && buscarUsuario(uid)) {
      estado = DEVOLUCAO;
      inicioEtapa = millis();
          Serial.println("Modo DEVOLUCAO: aproxime o RFID do equipamento.");
    } else {
      Serial.println("RFID nao cadastrado ou falha na consulta.");
      voltarInicio();
    }
  } else if (estado == AGUARDANDO_ALUNO) {
    if (buscarUsuario(uid)) {
      // Revalidar disponibilidade antes de gravar.
      bool disponivel = pegarStatusEquipamento(produtoSelecionado);
      if (async_client.lastError().code() == 0 && disponivel) {
        registrarEmprestimo(produtoSelecionado, uidProdutoSelecionado);
      } else {
        Serial.println("Equipamento indisponivel ou falha de consulta. Cancelado.");
      }
      voltarInicio();
    } else {
      Serial.println("Aluno nao identificado. Tente outro cartao ou reinicie.");
    }
  } else if (estado == DEVOLUCAO) {
    String equipamento = identificarEquipamento(uid);
    if (equipamento.length() > 0) {
      tentarDevolucao(equipamento);
    } else {
      Serial.println("Equipamento nao identificado. Tente novamente.");
    }
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  delay(350);
}