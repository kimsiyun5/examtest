// 필요한 헤더 파일 포함
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

// 클라이언트 구조체 정의
typedef struct s_client {
  int fd;                // 클라이언트 소켓 파일 디스크립터
  int id;                // 클라이언트 고유 ID
  char *buf;             // 클라이언트 메시지 버퍼
  struct s_client *next; // 다음 클라이언트를 가리키는 포인터
} t_client;

// 전역 변수 선언
t_client *clients = NULL; // 클라이언트 연결 리스트
int next_id = 0;          // 다음 클라이언트에게 할당할 ID
fd_set active_fds;        // 활성 파일 디스크립터 세트
int server_socket;        // 서버 소켓

// 치명적 오류 발생 시 처리하는 함수
void fatal_error() {
  char *msg = "Fatal error\n";
  write(2, msg, strlen(msg)); // 표준 에러에 메시지 출력
  exit(1);                    // 프로그램 종료
}

// 가장 큰 파일 디스크립터 값을 반환하는 함수
int get_max_fd() {
  int max_fd = server_socket;  // 서버 소켓으로 초기화
  t_client *current = clients; // 클라이언트 리스트 순회 시작

  // 모든 클라이언트를 순회하며 가장 큰 fd 값 찾기
  while (current) {
    if (current->fd > max_fd)
      max_fd = current->fd;
    current = current->next;
  }
  return max_fd;
}

// 두 문자열을 이어붙이는 함수
char *str_join(char *s1, char *s2) {
  char *newbuf;
  int len;

  // 문자열 길이 계산
  if (s1 == NULL)
    len = strlen(s2);
  else if (s2 == NULL)
    len = strlen(s1);
  else
    len = strlen(s1) + strlen(s2);

  // 새 버퍼 메모리 할당
  newbuf = malloc(sizeof(*newbuf) * (len + 1));
  if (newbuf == NULL)
    fatal_error();

  // 두 문자열 복사
  int i = 0;
  while (s1 && *s1)
    newbuf[i++] = *s1++;
  while (s2 && *s2)
    newbuf[i++] = *s2++;
  newbuf[i] = '\0'; // 문자열 종료 문자 추가

  return (newbuf);
}

// 문자열에서 줄바꿈 문자의 위치를 찾는 함수
int find_newline(char *buf) {
  int i = 0;
  // 문자열을 순회하며 개행 문자 검색
  while (buf[i]) {
    if (buf[i] == '\n')
      return i; // 개행 문자 위치 반환
    i++;
  }
  return -1; // 개행 문자가 없으면 -1 반환
}

// 메시지를 모든 클라이언트에게 브로드캐스트하는 함수
void broadcast(int send_fd, char *msg) {
  t_client *current = clients;
  // 모든 클라이언트를 순회
  while (current) {
    // 메시지를 보낸 클라이언트를 제외한 모든 클라이언트에게 전송
    if (current->fd != send_fd)
      send(current->fd, msg, strlen(msg), 0);
    current = current->next;
  }
}

void add_client() {
  // 클라이언트 연결 정보
  struct sockaddr_in client_addr;
  socklen_t addr_len = sizeof(client_addr);

  // 새 클라이언트 연결 수락
  int client_socket =
      accept(server_socket, (struct sockaddr *)&client_addr, &addr_len);
  if (client_socket < 0)
    return;

  // 새 클라이언트를 위한 메모리 할당
  t_client *new_client = malloc(sizeof(t_client));
  if (!new_client)
    fatal_error();

  // 클라이언트 데이터 초기화
  new_client->fd = client_socket;
  new_client->id = next_id++;
  new_client->buf = NULL;
  new_client->next = NULL;

  // 연결 리스트에 클라이언트 추가
  if (clients == NULL) {
    // 첫 번째 클라이언트
    clients = new_client;
  } else {
    // 리스트 끝에 추가
    t_client *current = clients;
    while (current->next)
      current = current->next;
    current->next = new_client;
  }

  // 모니터링할 파일 디스크립터에 클라이언트 소켓 추가
  FD_SET(client_socket, &active_fds);

  // 클라이언트 입장 메시지 브로드캐스트
  char msg[100];
  sprintf(msg, "server: client %d just arrived\n", new_client->id);
  broadcast(new_client->fd, msg);
}

// 클라이언트를 제거하는 함수
void remove_client(int fd) {
  t_client *current = clients;
  t_client *prev = NULL;

  // 제거할 클라이언트 검색
  while (current && current->fd != fd) {
    prev = current;
    current = current->next;
  }

  if (current == NULL)
    return;

  // 연결 리스트에서 클라이언트 제거
  if (prev)
    prev->next = current->next;
  else
    clients = clients->next;

  // 클라이언트의 메시지 버퍼 처리
  if (current->buf) {
    char *temp_buf = malloc(sizeof(char) * (strlen(current->buf) + 100));
    if (!temp_buf)
      fatal_error();
    sprintf(temp_buf, "client %d: %s\n", current->id, current->buf);

    free(temp_buf);
    free(current->buf);
  }

  // 클라이언트 퇴장 메시지 브로드캐스트
  char msg[100];
  sprintf(msg, "server: client %d just left\n", current->id);
  broadcast(current->fd, msg);

  // 파일 디스크립터 세트에서 제거 및 메모리 해제
  FD_CLR(current->fd, &active_fds);
  close(current->fd);
  free(current);
}

// 클라이언트로부터 받은 데이터를 처리하는 함수
void process_buf(t_client *client) {
  // 클라이언트로부터 데이터 수신
  char buffer[1024];
  int bytes_read = recv(client->fd, buffer, sizeof(buffer) - 1, 0);
  if (bytes_read <= 0) {
    remove_client(client->fd); // 연결 종료 시 클라이언트 제거
    return;
  }
  buffer[bytes_read] = '\0'; // 문자열 종료 문자 추가

  // 기존 버퍼와 새로 받은 데이터 병합
  char *temp_buf = str_join(client->buf, buffer);
  if (client->buf)
    free(client->buf);
  client->buf = temp_buf;

  // 줄바꿈 문자를 기준으로 메시지 처리
  int newline_idx = find_newline(client->buf);
  while (newline_idx != -1) {
    // 줄바꿈 문자를 NULL로 변경하여 메시지 분리
    client->buf[newline_idx] = '\0';

    // 메시지 형식 지정 및 브로드캐스트
    char *msg = malloc(sizeof(char) * (strlen(client->buf) + 100));
    if (msg == NULL)
      fatal_error();
    sprintf(msg, "client %d: %s\n", client->id, client->buf);
    broadcast(client->fd, msg);
    free(msg);

    // 버퍼에서 처리된 메시지 제거
    int i = 0;
    while (client->buf[newline_idx + 1 + i]) {
      client->buf[i] = client->buf[newline_idx + 1 + i];
      i++;
    }
    client->buf[i] = '\0';

    // 다음 메시지 검색
    newline_idx = find_newline(client->buf);
  }
}

// 메인 함수: 서버 초기화 및 실행
int main(int argc, char const *argv[]) {
  // 명령행 인수 검증
  if (argc != 2) {
    char *msg = "Wrong number of arguments\n";
    write(2, msg, strlen(msg));
    exit(1);
  }

  // 서버 소켓 생성
  server_socket = socket(AF_INET, SOCK_STREAM, 0);
  if (server_socket < 0)
    fatal_error();

  struct sockaddr_in server_addr;

  // 서버 주소 구조체 초기화
  memset(&server_addr, 0, sizeof(server_addr));
  // IP, PORT 설정
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(2130706433); // 127.0.0.1
  server_addr.sin_port = htons(atoi(argv[1]));     // 포트 번호

  // 소켓을 지정된 IP와 포트에 바인딩
  if ((bind(server_socket, (const struct sockaddr *)&server_addr,
            sizeof(server_addr))) != 0)
    fatal_error();

  // 클라이언트 연결 대기 큐 설정
  if (listen(server_socket, 10) != 0)
    fatal_error();

  // 파일 디스크립터 세트 초기화
  FD_ZERO(&active_fds);
  FD_SET(server_socket, &active_fds);

  // 서버 메인 루프
  while (1) {
    // select 함수를 위한 임시 세트
    fd_set read_fds = active_fds;
    int max_fd = get_max_fd();

    // 활성화된 파일 디스크립터 감시
    if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0)
      fatal_error();

    // 새 클라이언트 연결 요청 처리
    if (FD_ISSET(server_socket, &read_fds))
      add_client();

    // 기존 클라이언트의 데이터 처리
    t_client *current = clients;
    while (current) {
      t_client *next = current->next;

      if (FD_ISSET(current->fd, &read_fds))
        process_buf(current);
      current = next;
    }
  }

  return 0;
}
