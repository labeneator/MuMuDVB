/* 
 * mumudvb - UDP-ize a DVB transport stream.
 * 
 * (C) 2009 Brice DUBOST
 * 
 * The latest version can be found at http://mumudvb.braice.net
 * 
 * Copyright notice:
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/** @file
 * @brief File for HTTP unicast
 * @author Brice DUBOST
 * @date 2009
 */

/***********************
Todo list
 * implement a measurement of the "load"
 * implement channel by name
 * implement monitoring features

***********************/

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <poll.h>
#include <fcntl.h>



#include "unicast_http.h"
#include "mumudvb.h"


int 
unicast_send_streamed_channels_list (int number_of_channels, mumudvb_channel_t *channels, int Socket, char *host);
int 
unicast_send_streamed_channels_list_txt (int number_of_channels, mumudvb_channel_t *channels, int Socket);


/** @brief Accept an incoming connection
 *
 *
 * @param unicast_vars the unicast parameters
 */
int unicast_accept_connection(unicast_parameters_t *unicast_vars)
{

  unsigned int l;
  int tempSocket;
  struct sockaddr_in tempSocketAddrIn;
  l = sizeof(struct sockaddr);
  tempSocket = accept(unicast_vars->socketIn, (struct sockaddr *) &tempSocketAddrIn, &l);
  if (tempSocket < 0)
    {
      log_message(MSG_ERROR,"Unicast : Error when accepting the incoming connection : %s\n", strerror(errno));
      return -1;
    }
  log_message(MSG_DETAIL,"Unicast : New connection from %s:%d\n",inet_ntoa(tempSocketAddrIn.sin_addr), tempSocketAddrIn.sin_port);

  if( unicast_add_client(unicast_vars, tempSocketAddrIn, tempSocket)<0)
    {
      //We cannot create the client, we close the socket cleanly
      close(tempSocket);                 
      return -1;
    }

  //Now we set this socket to be non blocking because we poll it
  int flags;
  flags = fcntl(tempSocket, F_GETFL, 0);
  flags |= O_NONBLOCK;
  if (fcntl(tempSocket, F_SETFL, flags) < 0)
    {
      log_message(MSG_ERROR,"Set non blocking failed : %s\n",strerror(errno));
      return -1;
    }


  return tempSocket;

}

/** @brief Add a client to the chained list of clients
 * Will allocate the memory and fill the structure
 *
 * @param unicast_vars the unicast parameters
 * @param SocketAddr The socket address
 * @param Socket The socket number
 */
int unicast_add_client(unicast_parameters_t *unicast_vars, struct sockaddr_in SocketAddr, int Socket)
{

  unicast_client_t *client;
  unicast_client_t *prev_client;
  log_message(MSG_DEBUG,"Unicast : We create a client associated with %d\n",Socket);
  //We allocate a new client
  if(unicast_vars->clients==NULL)
    {
      client=unicast_vars->clients=malloc(sizeof(unicast_client_t));
      prev_client=NULL;
    }
  else
    {
      client=unicast_vars->clients;
      while(client->next!=NULL)
	client=client->next;
      client->next=malloc(sizeof(unicast_client_t));
      prev_client=client;
      client=client->next;
    }
  if(client==NULL)
    {
      log_message(MSG_ERROR,"Unicast : allocating a new client : MALLOC error");
      close(Socket);                 
      return -1;
    }

  //We fill the client data
  client->SocketAddr=SocketAddr;
  client->Socket=Socket;
  client->buffer=NULL;
  client->buffersize=0;
  client->bufferpos=0;
  client->channel=-1;
  client->consecutive_errors=0;
  client->next=NULL;
  client->prev=prev_client;
  client->chan_next=NULL;
  client->chan_prev=NULL;

  return 0;
}



/** @brief Delete a client to the chained list of clients and in the associated channel
 * This function close the socket of the client
 * remove it from the associated channel if there is one
 * and free the memory of the client (including the buffer)
 *
 * @param unicast_vars the unicast parameters
 * @param Socket the socket nubmer of the client we want to delete
 * @param channels the array of channels
 */
int unicast_del_client(unicast_parameters_t *unicast_vars, int Socket, mumudvb_channel_t *channels)
{
  unicast_client_t *client;
  unicast_client_t *prev_client,*next_client;

  client=unicast_find_client(unicast_vars,Socket);
  if(client==NULL)
    {
      log_message(MSG_ERROR,"Unicast : delete client : This should never happend, please contact\n");
      return -1;
    }
  log_message(MSG_DETAIL,"Unicast : We delete the client %s:%d, socket %d\n",inet_ntoa(client->SocketAddr.sin_addr), client->SocketAddr.sin_port, Socket);

  if (client->Socket >= 0)
    {			    
      close(client->Socket);                 
    }
  
  prev_client=client->prev;
  next_client=client->next;
  if(prev_client==NULL)
    {
      unicast_vars->clients=NULL;
    }
  else
    prev_client->next=client->next;

  //We delete the client in the channel
  if(client->channel!=-1)
    {
      log_message(MSG_DEBUG,"Unicast : We remove the client from the channel \"%s\"\n",channels[client->channel].name);

      if(client->chan_prev==NULL)
	channels[client->channel].clients=client->chan_next;
      else
	client->chan_prev->chan_next=client->chan_next;
    }


  if(client->buffer)
    free(client->buffer);
  free(client);

  return 0;
}

/** @brief Find a client with a specified socket
 * return NULL if no clients are found
 *
 * @param unicast_vars the unicast parameters
 * @param Socket the socket for wich we search the client
 */
unicast_client_t *unicast_find_client(unicast_parameters_t *unicast_vars, int Socket)
{
  unicast_client_t *client;

  if(unicast_vars->clients==NULL)
    return NULL;
  else
    {
      client=unicast_vars->clients;
      while(client!=NULL)
	{
	  if (client->Socket==Socket)
	    return client;
	  client=client->next;
	}
    }

  return NULL;
}





/** @brief Close an unicast connection and delete the client
 *
 * @param unicast_vars the unicast parameters
 * @param fds The polling file descriptors
 * @param Socket The socket of the client we want to disconnect
 * @param channels The channel list
 */
void unicast_close_connection(unicast_parameters_t *unicast_vars, fds_t *fds, int Socket, mumudvb_channel_t *channels)
{

  int actual_fd;
  actual_fd=0;
  //We find the FD correspondig to this client
  while((actual_fd<fds->pfdsnum) && (fds->pfds[actual_fd].fd!=Socket))
    actual_fd++;

  if(actual_fd==fds->pfdsnum)
    {
      log_message(MSG_ERROR,"Unicast : close connection : this should never happend, please contact\n");
      return;
    }

  log_message(MSG_DEBUG,"Unicast : We close the connection\n");
  //We delete the client
  unicast_del_client(unicast_vars, Socket, channels);
  //We move the last fd to the actual/deleted one, and decrease the number of fds by one
  fds->pfds[actual_fd].fd = fds->pfds[fds->pfdsnum-1].fd;
  fds->pfds[actual_fd].events = fds->pfds[fds->pfdsnum-1].events;
  fds->pfds[actual_fd].revents = fds->pfds[fds->pfdsnum-1].revents;
  fds->pfds[fds->pfdsnum-1].fd=0;
  fds->pfds[fds->pfdsnum-1].events=POLLIN|POLLPRI;
  fds->pfdsnum--;
  fds->pfds=realloc(fds->pfds,(fds->pfdsnum+1)*sizeof(struct pollfd));/**@todo check return value*/
  log_message(MSG_DEBUG,"Unicast : Number of clients : %d\n", fds->pfdsnum-2);

}




/** @brief Deal with an incoming message on the unicast client connection
 * This function will store and answer the HTTP requests
 *
 *
 * @param unicast_vars the unicast parameters
 */
int unicast_handle_message(unicast_parameters_t *unicast_vars, int fd, mumudvb_channel_t *channels, int number_of_channels)
{
  unicast_client_t *client;
  int received_len;
  

  client=unicast_find_client(unicast_vars,fd);
  if(client==NULL)
    {
      log_message(MSG_ERROR,"Unicast : This should never happend, please contact\n");
      return -1;
    }

  if((client->buffersize-client->bufferpos)<RECV_BUFFER_MULTIPLE)
    {
      client->buffer=realloc(client->buffer,(client->buffersize + RECV_BUFFER_MULTIPLE)*sizeof(char));
      client->buffersize += RECV_BUFFER_MULTIPLE;
      if(client->buffer==NULL)
	{
	  log_message(MSG_ERROR,"Unicast : Problem with realloc for the client buffer : %s\n",strerror(errno));
	  client->buffersize=0;
	  client->bufferpos=0;
	  return -1;
	}
    }

  received_len=recv(client->Socket, client->buffer+client->bufferpos, RECV_BUFFER_MULTIPLE, 0);

  if(received_len>0)
    {
      client->bufferpos+=received_len;
      log_message(MSG_DEBUG,"Unicast : We received %d, buffer len %d new buffer pos %d\n",received_len,client->buffersize, client->bufferpos);
      //log_message(MSG_DEBUG,"Unicast : Actual message :\n--------------\n%s\n----------\n",client->buffer);
    }

  if(received_len==-1)
    {
      log_message(MSG_ERROR,"Unicast : Problem with recv : %s\n",strerror(errno));
      return -1;
    }
  if(received_len==0)
    return -2; //To say to the main program to close the connection

  /***************** Now we parse the message to see if something was asked  *****************/

  //We search for the end of the HTTP request
  if(strstr(client->buffer, "\n\r\n\0"))
    {
      int pos,err404;
      char *substring=NULL;
      int requested_channel;
      int iRet;
      requested_channel=0;
      pos=0;
      err404=0;

      log_message(MSG_DEBUG,"Unicast : End of HTTP request, we parse it\n");
      
      if(strstr(client->buffer,"GET ")==client->buffer)
	{
	
	  //to implement : 
	  //Information ???
	  //GET /monitor/???

	  pos=4;
	  //Channel by number
	  //GET /bynumber/channelnumber
	  if(strstr(client->buffer +pos ,"/bynumber/")==(client->buffer +pos))
	    {
	      if(client->channel!=-1)
		{
		  log_message(MSG_INFO,"Unicast : A channel (%d) is already streamed to this client, it shouldn't ask for a new one without closing the connection, error 501\n",client->channel);
		  iRet=write(client->Socket,HTTP_501_REPLY, strlen(HTTP_501_REPLY)); //iRet is to make the copiler happy we will close the connection anyways
		  return -2; //to delete the client
		}
		
	      pos+=10;
	      substring = strtok (client->buffer+pos, " ");
	      if(substring == NULL)
		err404=1;
	      else
		{
		  requested_channel=atoi(substring);
		  if(requested_channel && requested_channel<=number_of_channels)
		    log_message(MSG_DEBUG,"Unicast : Channel by number, number %d\n",requested_channel);
		  else
		    {
		      log_message(MSG_INFO,"Unicast : Channel by number, number %d out of range\n",requested_channel);
		      err404=1;
		      requested_channel=0;
		    }
		}
	    }
	  //Channel by name
	  //GET /byname/channelname
	  else if(strstr(client->buffer +pos ,"/byname/")==(client->buffer +pos))
	    {
	      if(client->channel!=-1)
		{
		  log_message(MSG_INFO,"Unicast : A channel is already streamed to this client, it shouldn't ask for a new one without closing the connection, error 501\n");
		  iRet=write(client->Socket,HTTP_501_REPLY, strlen(HTTP_501_REPLY));//iRet is to make the copiler happy we will close the connection anyways
		  return -2; //to delete the client
		}
	      pos+=8;
	      log_message(MSG_DEBUG,"Unicast : Channel by number\n");
	      substring = strtok (client->buffer+pos, " ");
	      if(substring == NULL)
		err404=1;
	      else
		{
		  log_message(MSG_DEBUG,"Unicast : Channel by name, name %s\n",substring);
		  //search the channel
		  err404=1;//Temporary
		  /**@todo, implement the search without the spaces*/
		}
	    }
	  //Channels list
	  else if(strstr(client->buffer +pos ,"/channels_list.html")==(client->buffer +pos))
	    {
	      //We get the host name if availaible
	      char *hoststr;
	      hoststr=strstr(client->buffer ,"Host: ");
	      if(hoststr)
		{
		  substring = strtok (hoststr+6, "\r");
		}
	      else 
		substring=NULL;

	      unicast_send_streamed_channels_list (number_of_channels, channels, client->Socket, substring);
	      return -2; //We close the connection afterwards
	    }
	  //Channels list, text version
	  else if(strstr(client->buffer +pos ,"/channels_list.txt")==(client->buffer +pos))
	    {
	      unicast_send_streamed_channels_list_txt (number_of_channels, channels, client->Socket);
	      return -2; //We close the connection afterwards
	    }
	  //Not implemented path --> 404
	  else
	      err404=1;




	  if(err404)
	    {
	      log_message(MSG_INFO,"Unicast : Path not found i.e. 404\n");
	      iRet=write(client->Socket,HTTP_404_REPLY, strlen(HTTP_404_REPLY));//iRet is to make the copiler happy we will close the connection anyways
	      iRet=write(client->Socket,HTTP_404_REPLY_HTML, strlen(HTTP_404_REPLY_HTML));//iRet is to make the copiler happy we will close the connection anyways
	      return -2; //to delete the client
	    }

	  //We have found a channel, we add the client
	  /**@todo : add a limit in the number of simultaneous clients*/
	  if(requested_channel)
	    {
	      if(!channel_add_unicast_client(client,&channels[requested_channel-1]))
		client->channel=requested_channel-1;
	      else
		return -2;
	    }

	}
      else
	{
	  //We don't implement this http method, but if the client is already connected, we keep the connection
	  if(client->channel==-1)
	    {
	      log_message(MSG_INFO,"Unicast : Unhandled HTTP method : \"%s\", error 501\n",  strtok (client->buffer+pos, " "));
	      iRet=write(client->Socket,HTTP_501_REPLY, strlen(HTTP_501_REPLY));//iRet is to make the copiler happy we will close the connection anyways
	      return -2; //to delete the client
	    }
	  else
	    {
	      log_message(MSG_INFO,"Unicast : Unhandled HTTP method : \"%s\", error 501 but we keep the client connected\n",  strtok (client->buffer+pos, " "));
	      return 0;
	    }
	}
      //We don't need the buffer anymore
      free(client->buffer);
      client->buffer=NULL;
      client->bufferpos=0;
      client->buffersize=0;
    }

  return 0;
}


/** @brief This function add an unicast client to a channel
 *
 * @param client the client
 * @param channel the channel
 */
int channel_add_unicast_client(unicast_client_t *client,mumudvb_channel_t *channel)
{
  unicast_client_t *last_client;
  int iRet;
  
  log_message(MSG_INFO,"Unicast : We add the client %s:%d to the channel \"%s\"\n",inet_ntoa(client->SocketAddr.sin_addr), client->SocketAddr.sin_port,channel->name);

  iRet=write(client->Socket,HTTP_OK_REPLY, strlen(HTTP_OK_REPLY));
  if(iRet!=strlen(HTTP_OK_REPLY))
    {
      log_message(MSG_INFO,"Unicast : Error when sending the HTTP reply\n");
      return -1;
    }

  if(channel->clients==NULL)
    channel->clients=client;
  else
    {
      last_client=channel->clients;
      while(last_client->chan_next!=NULL)
	last_client=last_client->chan_next;
      last_client->chan_next=client;
      client->chan_prev=last_client;
    }
  return 0;
}


/** @brief Delete all the clients
 *
 * @param unicast_vars the unicast parameters
 * @param channels : the channels structure
 */
void unicast_freeing(unicast_parameters_t *unicast_vars, mumudvb_channel_t *channels)
{
  unicast_client_t *actual_client;
  unicast_client_t *next_client;

  for(actual_client=unicast_vars->clients; actual_client != NULL; actual_client=next_client)
    {
      next_client= actual_client->next;
      unicast_del_client(unicast_vars, actual_client->Socket, channels);
    }
}


/** @brief Send a basic html file containig the list of streamed channels
 *
 * @param number_of_channels the number of channels
 * @param channels the channels array
 * @param Socket the socket on wich the information have to be sent
 */
int 
unicast_send_streamed_channels_list (int number_of_channels, mumudvb_channel_t *channels, int Socket, char *host)
{
  int curr_channel;
  int iRet;
  char buffer[255];


  iRet=write(Socket,HTTP_OK_HTML_REPLY, strlen(HTTP_OK_HTML_REPLY));
  if(iRet!=strlen(HTTP_OK_HTML_REPLY))
    {
      log_message(MSG_INFO,"Unicast : Error when sending the HTTP reply\n");
      return -1;
    }
  iRet=write(Socket,HTTP_CHANNELS_REPLY_START, strlen(HTTP_CHANNELS_REPLY_START));
  if(iRet!=strlen(HTTP_CHANNELS_REPLY_START))
    {
      log_message(MSG_INFO,"Unicast : Error when sending the HTTP reply\n");
      return -1;
    }


  for (curr_channel = 0; curr_channel < number_of_channels; curr_channel++)
    if (channels[curr_channel].streamed_channel_old)
      {
	if(host)
	  iRet=snprintf(buffer, 255, "Channel number %d : %s<br>Unicast link : <a href=\"http://%s/bynumber/%d\">http://%s/bynumber/%d</a><br>Multicast ip : %s:%d<br><br>\r\n",
			curr_channel,
			channels[curr_channel].name,
			host,curr_channel+1,
			host,curr_channel+1,
			channels[curr_channel].ipOut,channels[curr_channel].portOut);
	else
	  iRet=snprintf(buffer, 255, "Channel number %d : \"%s\"<br>Multicast ip : %s:%d<br><br>\r\n",curr_channel,channels[curr_channel].name,channels[curr_channel].ipOut,channels[curr_channel].portOut);
     
	iRet=write(Socket,buffer, strlen(buffer));
	if(iRet!=(int)strlen(buffer)) //Cast to make compiler happy
	  {
	    log_message(MSG_INFO,"Unicast : Error when sending the HTTP reply\n");
	    return -1;
	  }
      }

  iRet=write(Socket,HTTP_CHANNELS_REPLY_END, strlen(HTTP_CHANNELS_REPLY_END));
  if(iRet!=strlen(HTTP_CHANNELS_REPLY_END))
    {
      log_message(MSG_INFO,"Unicast : Error when sending the HTTP reply\n");
      return -1;
    }


  return 0;
}



/** @brief Send a basic text file containig the list of streamed channels
 *
 * @param number_of_channels the number of channels
 * @param channels the channels array
 * @param Socket the socket on wich the information have to be sent
 */
int 
unicast_send_streamed_channels_list_txt (int number_of_channels, mumudvb_channel_t *channels, int Socket)
{
  int curr_channel;
  int iRet;
  char buffer[1024];
  unicast_client_t *unicast_client=NULL;
  int clients=0;


  iRet=write(Socket,HTTP_OK_TEXT_REPLY, strlen(HTTP_OK_TEXT_REPLY));
  if(iRet!=strlen(HTTP_OK_TEXT_REPLY))
    {
      log_message(MSG_INFO,"Unicast : Error when sending the HTTP reply\n");
      return -1;
    }
  iRet=write(Socket,HTTP_CHANNELS_REPLY_TEXT_START, strlen(HTTP_CHANNELS_REPLY_TEXT_START));
  if(iRet!=strlen(HTTP_CHANNELS_REPLY_TEXT_START))
    {
      log_message(MSG_INFO,"Unicast : Error when sending the HTTP reply\n");
      return -1;
    }

  //"#Channel number::name::sap playlist group::multicast ip::multicast port::num unicast clients::scrambling ratio\r\n"
  for (curr_channel = 0; curr_channel < number_of_channels; curr_channel++)
    if (channels[curr_channel].streamed_channel_old)
      {
	clients=0;
	unicast_client=channels[curr_channel].clients;
	while(unicast_client!=NULL)
	  {
	    unicast_client=unicast_client->chan_next;
	    clients++;
	  }
	iRet=snprintf(buffer, 1024, "%d::%s::%s::%s::%d::%d::%d\r\n",
		      curr_channel,
		      channels[curr_channel].name,
		      channels[curr_channel].sap_group,
		      channels[curr_channel].ipOut,
		      channels[curr_channel].portOut,
		      clients,
		      channels[curr_channel].ratio_scrambled);
				  
	iRet=write(Socket,buffer, strlen(buffer));
	if(iRet!=(int)strlen(buffer)) //Cast to make compiler happy
	  {
	    log_message(MSG_INFO,"Unicast : Error when sending the HTTP reply\n");
	    return -1;
	  }
      }

  iRet=write(Socket,"\r\n\r\n", strlen("\r\n\r\n"));
  if(iRet!=strlen("\r\n\r\n"))
    {
      log_message(MSG_INFO,"Unicast : Error when sending the HTTP reply\n");
      return -1;
    }


  return 0;
}