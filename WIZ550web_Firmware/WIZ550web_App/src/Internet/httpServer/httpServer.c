#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include "socket.h"
#include "wizchip_conf.h"

#include "httpServer.h"
#include "httpParser.h"
#include "httpUtil.h"

#ifdef	_USE_SDCARD_
#include "ff.h"
#else
#include "dataflash.h"
#endif

#define DATA_BUF_SIZE		2048

/*****************************************************************************
 * Private types/enumerations/variables
 ****************************************************************************/
st_http_socket **HTTPSock_Status;
static uint8_t HTTPSock_Num[_WIZCHIP_SOCK_NUM_] = {0, };
static st_http_request * http_request;				/**< Pointer to received HTTP request */
static st_http_request * parsed_http_request;		/**< Pointer to parsed HTTP request */
static uint8_t * http_response;						/**< Pointer to HTTP response */

/*****************************************************************************
 * Public types/enumerations/variables
 ****************************************************************************/
uint8_t * pHTTP_TX;
uint8_t * pHTTP_RX;

volatile uint32_t httpServer_tick_1s = 0;

#ifdef	_USE_SDCARD_
FIL **fs;	// FatFs File objects
FRESULT fr;	// FatFs function common result code
#endif

/*****************************************************************************
 * Private functions
 ****************************************************************************/
void httpServer_Sockinit(uint8_t cnt, uint8_t * socklist);
static uint8_t getHTTPSocketNum(uint8_t seqnum);
static int8_t getHTTPSequenceNum(uint8_t socket);
static int8_t http_disconnect(uint8_t sn);

static void http_process_handler(uint8_t s, st_http_request * p_http_request);
static void send_http_response_header(uint8_t s, uint8_t content_type, uint32_t body_len, uint16_t http_status);
static void send_http_response_body(uint8_t s, st_http_request * p_http_request, uint8_t * buf, uint32_t start_addr, uint32_t file_len);
static void send_http_response_cgi(uint8_t s, uint8_t * buf, uint8_t * http_body, uint16_t file_len);

/*****************************************************************************
 * Public functions
 ****************************************************************************/
// Callback functions definition: MCU Reset / WDT Reset
void default_mcu_reset(void) {;}
void default_wdt_reset(void) {;}
void (*HTTPServer_ReStart)(void) = default_mcu_reset;
void (*HTTPServer_WDT_Reset)(void) = default_wdt_reset;

void httpServer_Sockinit(uint8_t cnt, uint8_t * socklist)
{
	uint8_t i;
	// Dynamic allocation : structure for send the content (each socket)
	HTTPSock_Status = (st_http_socket **)malloc(sizeof(st_http_socket *) * cnt);

	for(i = 0; i < cnt; i++)
	{
		HTTPSock_Status[i] = (st_http_socket *)malloc(sizeof(st_http_socket));

		// Structure initialization
		HTTPSock_Status[i]->sock_status = STATE_HTTP_IDLE;
		HTTPSock_Status[i]->file_start = 0;
		HTTPSock_Status[i]->file_len = 0;
		HTTPSock_Status[i]->file_offset = 0;

		// Mapping the H/W socket numbers to the sequence numbers
		HTTPSock_Num[i] = socklist[i];
	}

#ifdef	_USE_SDCARD_
	// FAT File system structure 'fs' allocation for each HW sockets
	fs = (FIL **)malloc(sizeof(FIL *) * cnt);
	for(i = 0; i < cnt; i++) fs[i] = (FIL *)malloc(sizeof(FIL));
#endif
}

static uint8_t getHTTPSocketNum(uint8_t seqnum)
{
	// Return the 'H/W socket number' corresponding to the sequence number
	return HTTPSock_Num[seqnum];
}

static int8_t getHTTPSequenceNum(uint8_t socket)
{
	uint8_t i;

	for(i = 0; i < _WIZCHIP_SOCK_NUM_; i++)
		if(HTTPSock_Num[i] == socket) return i;

	return -1;
}


void httpServer_init(uint8_t * tx_buf, uint8_t * rx_buf, uint8_t cnt, uint8_t * socklist)
{
	// User's shared buffer
	pHTTP_TX = tx_buf;
	pHTTP_RX = rx_buf;

	// Structure dynamic allocation / socket number mapping
	httpServer_Sockinit(cnt, socklist);
}


/* Register the call back functions for HTTP Server */
void reg_httpServer_cbfunc(void(*mcu_reset)(void), void(*wdt_reset)(void))
{
	// Callback: HW Reset and WDT reset function for each MCU platforms
	if(mcu_reset) HTTPServer_ReStart = mcu_reset;
	if(wdt_reset) HTTPServer_WDT_Reset = wdt_reset;
}


void httpServer_run(uint8_t seqnum)
{
	uint8_t s;	// socket number
	uint16_t len;
	uint32_t gettime = 0;

#ifdef _HTTPSERVER_DEBUG_
	uint8_t destip[4] = {0, };
	uint16_t destport = 0;
#endif

	http_request = (st_http_request *)pHTTP_RX;		// Structure of HTTP Request
	parsed_http_request = (st_http_request *)pHTTP_TX;

	// Get the H/W socket number
	s = getHTTPSocketNum(seqnum);

	/* HTTP Service Start */
	switch(getSn_SR(s))
	{
		case SOCK_ESTABLISHED:
			// Interrupt clear
			if(getSn_IR(s) & Sn_IR_CON)
			{
				setSn_IR(s, Sn_IR_CON);
			}

			// HTTP Process states
			switch(HTTPSock_Status[seqnum]->sock_status)
			{

				case STATE_HTTP_IDLE :
					if ((len = getSn_RX_RSR(s)) > 0)
					{
						if (len > DATA_BUF_SIZE) len = DATA_BUF_SIZE;
						len = recv(s, (uint8_t *)http_request, len);

						*(((uint8_t *)http_request) + len) = '\0';

						parse_http_request(parsed_http_request, (uint8_t *)http_request);
//						HTTPSock_Status[seqnum]->sock_status = STATE_HTTP_REQ_DONE;	// ## State [STATE_HTTP_REQ_DONE] is contained in the [STATE_HTTP_IDLE].
#ifdef _HTTPSERVER_DEBUG_
						getSn_DIPR(s, destip);
						destport = getSn_DPORT(s);
						printf("\r\n");
						printf("> HTTPSocket[%d] : HTTP Request received ", s);
						printf("from %d.%d.%d.%d : %d\r\n", destip[0], destip[1], destip[2], destip[3], destport);
#endif
/*
				case STATE_HTTP_REQ_INPROC : // ==> No use; will be implemented later
#ifdef _HTTPSERVER_DEBUG_
					printf("> HTTPSocket[%d] : [State] STATE_HTTP_REQ_INPROC\r\n", s);
#endif
*/
//				case STATE_HTTP_REQ_DONE :											// ## State [STATE_HTTP_REQ_DONE] is contained in the [STATE_HTTP_IDLE].
					// Generate and send the HTTP Response created using the parsed HTTP Request
					/*
					 * HTTPSock_Status[seqnum]->file_len : if the HTTP response send ended, clear this value
					 *
					 * */
#ifdef _HTTPSERVER_DEBUG_
						printf("> HTTPSocket[%d] : [State] STATE_HTTP_REQ_DONE\r\n", s);
#endif
						// HTTP 'response' handler; includes send_http_response_header / body function
						http_process_handler(s, parsed_http_request);

						gettime = get_httpServer_timecount();
						// Check the TX socket buffer for End of HTTP response sends
						while(getSn_TX_FSR(s) != (getSn_TXBUF_SIZE(s)*1024))
						{
							if((get_httpServer_timecount() - gettime) > 3)
							{
#ifdef _HTTPSERVER_DEBUG_
								printf("> HTTPSocket[%d] : [State] STATE_HTTP_REQ_DONE: TX Buffer clear timeout\r\n", s);
#endif
								break;
							}
						}

						if(HTTPSock_Status[seqnum]->file_len > 0) HTTPSock_Status[seqnum]->sock_status = STATE_HTTP_RES_INPROC;
						else HTTPSock_Status[seqnum]->sock_status = STATE_HTTP_RES_DONE; // Send the 'HTTP response' end
					}

					break;

				case STATE_HTTP_RES_INPROC :
					/* Repeat: Send the remain parts of HTTP responses */
#ifdef _HTTPSERVER_DEBUG_
					printf("> HTTPSocket[%d] : [State] STATE_HTTP_RES_INPROC\r\n", s);
#endif
					// Repeatedly send remaining data to client
					send_http_response_body(s, parsed_http_request, http_response, 0, 0);

					if(HTTPSock_Status[seqnum]->file_len == 0) HTTPSock_Status[seqnum]->sock_status = STATE_HTTP_RES_DONE;
					break;

				case STATE_HTTP_RES_DONE :
#ifdef _HTTPSERVER_DEBUG_
					printf("> HTTPSocket[%d] : [State] STATE_HTTP_RES_DONE\r\n", s);
#endif
					// Socket file info structure re-initialize
					HTTPSock_Status[seqnum]->file_len = 0;
					HTTPSock_Status[seqnum]->file_offset = 0;
					HTTPSock_Status[seqnum]->file_start = 0;
					HTTPSock_Status[seqnum]->sock_status = STATE_HTTP_IDLE;

#ifdef _USE_SDCARD_
					f_close(fs[seqnum]);
#endif
#ifdef _USE_WATCHDOG_
/*
#ifdef _HTTPSERVER_DEBUG_
					printf("> HTTPSocket[%d] : Watchdog timer reset #1\r\n", s);
#endif
*/
					HTTPServer_WDT_Reset();
#endif
					http_disconnect(s);
					break;

				default :
					break;
			}
			break;

		case SOCK_CLOSE_WAIT:
#ifdef _HTTPSERVER_DEBUG_
		printf("> HTTPSocket[%d] : ClOSE_WAIT\r\n", s);	// if a peer requests to close the current connection
#endif
			disconnect(s);
			break;

		case SOCK_CLOSED:
#ifdef _HTTPSERVER_DEBUG_
			printf("> HTTPSocket[%d] : CLOSED\r\n", s);
#endif
			if(socket(s, Sn_MR_TCP, HTTP_SERVER_PORT, 0x00) == s)    /* Reinitialize the socket */
			{
#ifdef _HTTPSERVER_DEBUG_
				printf("> HTTPSocket[%d] : OPEN\r\n", s);
#endif
			}
			break;

		case SOCK_INIT:
			listen(s);
			break;

		case SOCK_LISTEN:
			break;

		default :
			break;

	} // end of switch

#ifdef _USE_WATCHDOG_
/*
#ifdef _HTTPSERVER_DEBUG_
	printf("> HTTPSocket[%d] : Watchdog timer reset #2\r\n", s);
#endif
*/
	HTTPServer_WDT_Reset();
#endif
}

////////////////////////////////////////////
// Private Functions
////////////////////////////////////////////
static void send_http_response_header(uint8_t s, uint8_t content_type, uint32_t body_len, uint16_t http_status)
{
	switch(http_status)
	{
		case STATUS_OK: 		// HTTP/1.1 200 OK
			if((content_type != PTYPE_CGI) && (content_type != PTYPE_XML)) // CGI/XML type request does not respond HTTP header
			{
#ifdef _HTTPSERVER_DEBUG_
			printf("> HTTPSocket[%d] : HTTP Response Header - STATUS_OK\r\n", s);
#endif
				make_http_response_head((char*)http_response, content_type, body_len);
			}
			else
			{
#ifdef _HTTPSERVER_DEBUG_
			printf("> HTTPSocket[%d] : HTTP Response Header - NONE / CGI or XML\r\n", s);
#endif
				// CGI/XML type request does not respond HTTP header to client
				http_status = 0;
			}
			break;
		case STATUS_BAD_REQ: 	// HTTP/1.1 400 OK
#ifdef _HTTPSERVER_DEBUG_
			printf("> HTTPSocket[%d] : HTTP Response Header - STATUS_BAD_REQ\r\n", s);
#endif
			memcpy(http_response, ERROR_REQUEST_PAGE, sizeof(ERROR_REQUEST_PAGE));
			break;
		case STATUS_NOT_FOUND:	// HTTP/1.1 404 Not Found
#ifdef _HTTPSERVER_DEBUG_
			printf("> HTTPSocket[%d] : HTTP Response Header - STATUS_NOT_FOUND\r\n", s);
#endif
			memcpy(http_response, ERROR_HTML_PAGE, sizeof(ERROR_HTML_PAGE));
			break;
		default:
			break;
	}

	// Send the HTTP Response 'header'
	if(http_status)
	{
#ifdef _HTTPSERVER_DEBUG_
		printf("> HTTPSocket[%d] : [Send] HTTP Response Header [ %d ]byte\r\n", s, (uint16)strlen((char *)http_response));
#endif
		send(s, http_response, strlen((char *)http_response));
	}
}


static void send_http_response_body(uint8_t s, st_http_request * p_http_request, uint8_t * buf, uint32_t start_addr, uint32_t file_len)
{
	int8_t get_seqnum;
	uint32_t send_len;

	uint8_t flag_datasend_end = 0;

#ifdef _USE_SDCARD_
	uint16_t blocklen;
#else
	uint32_t addr = 0;
#endif

	if((get_seqnum = getHTTPSequenceNum(s)) == -1) return; // exception handling; invalid number

	// Send the HTTP Response 'body'; requested file
	if(!HTTPSock_Status[get_seqnum]->file_len) // first part
	{
		if (file_len > DATA_BUF_SIZE - 1)
		{
			HTTPSock_Status[get_seqnum]->file_start = start_addr;
			HTTPSock_Status[get_seqnum]->file_len = file_len;
			send_len = DATA_BUF_SIZE - 1;

			HTTPSock_Status[get_seqnum]->file_offset = send_len;
#ifdef _HTTPSERVER_DEBUG_
			printf("> HTTPSocket[%d] : HTTP Response body - file len [ %ld ]byte / type [%d]\r\n", s, file_len, p_http_request->TYPE);
			printf("> HTTPSocket[%d] : HTTP Response body - offset [ %ld ]\r\n", s, HTTPSock_Status[get_seqnum]->file_offset);
#endif
		}
		else
		{
			// Send process end
			send_len = file_len;

#ifdef _HTTPSERVER_DEBUG_
			printf("> HTTPSocket[%d] : HTTP Response body - file len [ %ld ]byte / type [%d]\r\n", s, file_len, p_http_request->TYPE);
			printf("> HTTPSocket[%d] : HTTP Response end - send len [ %ld ]byte\r\n", s, send_len);
#endif
		}
#ifdef _USE_FLASH_
		addr = start_addr;
#endif
	}
	else // remained parts
	{
#ifdef _USE_FLASH_
		addr = HTTPSock_Status[get_seqnum]->file_start + HTTPSock_Status[get_seqnum]->file_offset;
#endif
		send_len = HTTPSock_Status[get_seqnum]->file_len - HTTPSock_Status[get_seqnum]->file_offset;

		if(send_len > DATA_BUF_SIZE - 1)
		{
			send_len = DATA_BUF_SIZE - 1;
			HTTPSock_Status[get_seqnum]->file_offset += send_len;
		}
		else
		{
#ifdef _HTTPSERVER_DEBUG_
			printf("> HTTPSocket[%d] : HTTP Response end - file len [ %ld ]byte\r\n", s, HTTPSock_Status[get_seqnum]->file_len);
#endif
			// Send process end
			flag_datasend_end = 1;
		}
#ifdef _HTTPSERVER_DEBUG_
			printf("> HTTPSocket[%d] : HTTP Response body - send len [ %ld ]byte\r\n", s, send_len);
#endif

#ifdef _HTTPSERVER_DEBUG_
			printf("> HTTPSocket[%d] : HTTP Response body - offset [ %ld ]\r\n", s, HTTPSock_Status[get_seqnum]->file_offset);
#endif
	}

#ifdef _USE_SDCARD_
	// Data read from SD Card
	fr = f_read(fs[get_seqnum], &buf[0], send_len, (void *)&blocklen);
	if(fr != FR_OK)
	{
		send_len = 0;
#ifdef _HTTPSERVER_DEBUG_
		printf("> HTTPSocket[%d] : [FatFs] Error code return: %d / HTTP Send Failed\r\n", s, fr);
#endif
	}
	else
	{
		*(buf+send_len+1) = 0; // Insert '/0' for string operation
	}
#else
	// Data read from external data flash memory
	read_from_flashbuf(addr, &buf[0], send_len);
	*(buf+send_len+1) = 0; // Insert '/0' for string operation
#endif

	// Requested content send to HTTP client
#ifdef _HTTPSERVER_DEBUG_
	printf("> HTTPSocket[%d] : [Send] HTTP Response body [ %ld ]byte\r\n", s, send_len);
#endif
	send(s, buf, send_len);

	if(flag_datasend_end)
	{
		HTTPSock_Status[get_seqnum]->file_start = 0;
		HTTPSock_Status[get_seqnum]->file_len = 0;
		HTTPSock_Status[get_seqnum]->file_offset = 0;
		flag_datasend_end = 0;
	}
}

static void send_http_response_cgi(uint8_t s, uint8_t * buf, uint8_t * http_body, uint16_t file_len)
{
	uint16_t send_len = 0;

#ifdef _HTTPSERVER_DEBUG_
	printf("> HTTPSocket[%d] : HTTP Response Header + Body - CGI\r\n", s);
#endif
	send_len = sprintf((char *)buf, "%s%d\r\n\r\n%s", RES_CGIHEAD_OK, file_len, http_body);
#ifdef _HTTPSERVER_DEBUG_
	printf("> HTTPSocket[%d] : HTTP Response Header + Body - send len [ %d ]byte\r\n", s, send_len);
#endif

	send(s, buf, send_len);
}


static int8_t http_disconnect(uint8_t sn)
{
	setSn_CR(sn,Sn_CR_DISCON);
	/* wait to process the command... */
	while(getSn_CR(sn));

	return SOCK_OK;
}


static void http_process_handler(uint8_t s, st_http_request * p_http_request)
{
	uint8_t * uri_name;
	uint32_t content_addr = 0;
	uint32_t file_len = 0;

	uint8_t post_name[32]={0x00,};	// POST method request file name

#ifdef _WILL_BE_IM_
	uint32_t content_len = 0;
	uint16_t post_len = 0; 			// POST
	uint8_t sub[10];				// POST
#endif

	uint16_t http_status;
	int8_t get_seqnum;
	uint8_t content_found;

	// ## for Debugging
	//uint16_t i;

	if((get_seqnum = getHTTPSequenceNum(s)) == -1) return; // exception handling; invalid number

	http_status = 0;
	http_response = pHTTP_RX;
	file_len = 0;

	//method Analyze
	switch (p_http_request->METHOD)
	{
		case METHOD_ERR :
			http_status = STATUS_BAD_REQ;
			send_http_response_header(s, 0, 0, http_status);
			break;

		case METHOD_HEAD :
		case METHOD_GET :
			uri_name = get_http_uri_name(p_http_request->URI);
			if (!strcmp((char *)uri_name, "/")) strcpy((char *)uri_name, INITIAL_WEBPAGE);	// If URI is "/", respond by index.html
			if (!strcmp((char *)uri_name, "mobile")) strcpy((char *)uri_name, MOBILE_INITIAL_WEBPAGE);
			find_http_uri_type(&p_http_request->TYPE, uri_name);	//Check file type (HTML, TEXT, GIF, JPEG are included)

#ifdef _HTTPSERVER_DEBUG_
			printf("\r\n> HTTPSocket[%d] : HTTP Method GET\r\n", s);
			printf("> HTTPSocket[%d] : Request Type = %d\r\n", s, p_http_request->TYPE);
			printf("> HTTPSocket[%d] : Request URI = %s\r\n", s, uri_name);
#endif

			if(p_http_request->TYPE == PTYPE_CGI)
			{
				content_found = http_get_cgi_handler(uri_name, pHTTP_TX, &file_len);
				if(content_found && (file_len <= (2048-(strlen(RES_CGIHEAD_OK)+8))))
				{
					send_http_response_cgi(s, http_response, pHTTP_TX, (uint16_t)file_len);
				}
				else
				{
					send_http_response_header(s, PTYPE_CGI, 0, STATUS_NOT_FOUND);
				}
			}
			else
			{	// Not CGI request, Web content in 'SD card' or 'Data flash' requested
#ifdef _USE_SDCARD_
				if((fr = f_open(fs[get_seqnum], (const char *)uri_name, FA_READ)) == 0)
				{
					content_found = 1; // file open succeed

					file_len = fs[get_seqnum]->fsize;
					content_addr = fs[get_seqnum]->sclust;
				}
				else
				{
#ifdef _HTTPSERVER_DEBUG_
					printf("> HTTPSocket[%d] : [FatFs] Error code return: %d\r\n", s, fr);
#endif
					content_found = 0; // file open failed
				}
#endif
				if(!content_found)
				{
#ifdef _HTTPSERVER_DEBUG_
					printf("> HTTPSocket[%d] : Unknown Page Request\r\n", s);
#endif
					http_status = STATUS_NOT_FOUND;
				}
				else
				{
#ifdef _HTTPSERVER_DEBUG_
					printf("> HTTPSocket[%d] : Find Content [%s] ok - Start [%ld] len [ %ld ]byte\r\n", s, uri_name, content_addr, file_len);
#endif
					http_status = STATUS_OK;
				}

				// Send HTTP header
				if(http_status)
				{
#ifdef _HTTPSERVER_DEBUG_
					printf("> HTTPSocket[%d] : Requested content len = [ %ld ]byte\r\n", s, file_len);
#endif
					send_http_response_header(s, p_http_request->TYPE, file_len, http_status);
				}

				// Send HTTP body (content)
				if(http_status == STATUS_OK)
				{
					 send_http_response_body(s, p_http_request, http_response, content_addr, file_len);
				}
			}
			break;

		case METHOD_POST :
			mid((char *)p_http_request->URI, "/", " HTTP", (char *)post_name);
			uri_name = post_name;
			find_http_uri_type(&p_http_request->TYPE, uri_name);	//Check file type (HTML, TEXT, GIF, JPEG are included)

#ifdef _HTTPSERVER_DEBUG_
			printf("\r\n> HTTPSocket[%d] : HTTP Method POST\r\n", s);
			printf("> HTTPSocket[%d] : Request URI = %s ", s, post_name);
			printf("Type = %d\r\n", p_http_request->TYPE);
#endif

			////////////////////////////////////////////////////////////////////////////////
			// If received the only HTTP header, try to receive the remained POST content
	        ////////////////////////////////////////////////////////////////////////////////
			//
			// This code NEEDS test more.
#ifdef _WILL_BE_IM_
			if(*(((uint8*)p_http_request->URI)+strlen((char*)p_http_request->URI)-1) == 0x0a) //only http header
	        {
				// Get Content-Length
				mid((char*)p_http_request->URI, "Content-Length: ", "\r\n", (char *)sub);
				content_len = ATOI(sub, 10);
#ifdef _HTTPSERVER_DEBUG_
				printf("> HTTPSocket[%d] : POST Content-length [ %ld ]byte\r\n", s, content_len);
#endif

				if(content_len > 2048) content_len = 2048;
				while(post_len != content_len)
				{

					post_len = getSn_RX_RSR(s);
#ifdef _HTTPSERVER_DEBUG_
					printf("> HTTPSocket[%d] : Received length (socket buffer) = %d\r\n", s, post_len);
#endif
					if(post_len >= content_len)
					{
						post_len = recv(s, (uint8_t *)(p_http_request)+strlen((char*)p_http_request->URI)+2, (uint16_t)content_len);
					}
					//printf("\r\nrx_len=%d %s\r\n",len,(char*) http_request->URI);

					//a timeout is needed!
#if 0
					if(cgi_post_wait_time>=1)//if post data does not come within 5 minutes
					{
#ifdef _HTTPSERVER_DEBUG_
						printf("pwd page response timeout\r\n");
#endif
						cgi_post_wait_time=0;
						return;
					}
#endif
				}
			}
#endif

			if(p_http_request->TYPE == PTYPE_CGI)	// HTTP POST Method; CGI Process
			{
				content_found = http_post_cgi_handler(post_name, p_http_request, http_response, &file_len);
#ifdef _HTTPSERVER_DEBUG_
				printf("> HTTPSocket[%d] : [CGI: %s] / Response len [ %ld ]byte\r\n", s, content_found?"Content found":"Content not found", file_len);
#endif
				if(content_found && (file_len <= (2048-(strlen(RES_CGIHEAD_OK)+8))))
				{
					send_http_response_cgi(s, pHTTP_TX, http_response, (uint16_t)file_len);

					// Reset H/W for apply the changed configuration information
					if(content_found == HTTP_RESET) HTTPServer_ReStart();
				}
				else
				{
					send_http_response_header(s, PTYPE_CGI, 0, STATUS_NOT_FOUND);
				}
			}
			else	// HTTP POST Method; Content not found
			{
				send_http_response_header(s, 0, 0, STATUS_NOT_FOUND);
			}
			break;

		default :
			http_status = STATUS_BAD_REQ;
			send_http_response_header(s, 0, 0, http_status);
			break;
	}
}

void httpServer_time_handler(void)
{
	httpServer_tick_1s++;
}

uint32_t get_httpServer_timecount(void)
{
	return httpServer_tick_1s;
}
