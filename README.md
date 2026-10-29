# LabControl — Empréstimo e devolução de equipamentos

Projeto acadêmico de Engenharia da Computação para controle de equipamentos de laboratório usando ESP32, RFID RC522 e Firebase Realtime Database.

## Funcionalidades
- Identificação de usuários e equipamentos via RFID.
- Consulta de disponibilidade.
- Registro de empréstimos e devoluções.
- Confirmação por botões SIM/NÃO.
- Registro de ocorrências na devolução.
- Registro de datas e horários via NTP.

## Tecnologias
ESP32, Arduino/C++, MFRC522, FirebaseClient, Firebase Realtime Database e Wi-Fi.

## Hardware e pinos (conforme este arquivo)
- RFID RC522: SS GPIO 5, RST GPIO 22, SPI padrão da placa.
- Botão SIM: GPIO 26; botão NÃO: GPIO 27 (INPUT_PULLUP).
- Monitor Serial: 9600 baud.

> Confirme os pinos na montagem real antes de utilizar.

## Como configurar
1. Instale o suporte ESP32 e as bibliotecas `FirebaseClient` e `MFRC522` no Arduino IDE.
2. Copie `firmware/LabControl/config.example.h` para `firmware/LabControl/config.h`.
3. Preencha `config.h` localmente com suas credenciais de teste. **Não publique esse arquivo.**
4. Abra `firmware/LabControl/LabControl.ino`, selecione sua placa ESP32 e a porta serial.
5. Compile, envie à placa e abra o Monitor Serial a 9600 baud.

## Segurança e limitações
- **Troque as senhas do Wi-Fi e da conta Firebase usadas na versão original antes de publicar ou distribuir.**
- Configure regras de segurança no Firebase; não habilite leitura/escrita pública.
- O firmware usa `ssl_client.setInsecure()` e não valida o certificado TLS. Substitua por validação adequada antes de uso em produção.
- O fluxo de empréstimo não é uma transação atômica entre caminhos do banco de dados.
- Este projeto demonstra integração de sistemas e IoT, **não** implementação de Machine Learning.

## Próximas melhorias
- Adicionar documentação do painel web e capturas de tela sem dados pessoais.
- Testes de falhas de rede e regras de consistência de empréstimos.
- Revisar TLS, permissões e sincronização de dados.

## Autor
Julio Cesar Pacheco Arduini — Engenharia da Computação.
