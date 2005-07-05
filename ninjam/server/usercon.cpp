#ifdef _WIN32
#include <windows.h>
#else
#include <stdlib.h>
#include <string.h>
#endif

#include <ctype.h>

#include "usercon.h"
#include "../mpb.h"

#include "../../WDL/rng.h"
#include "../../WDL/sha.h"


#ifdef _WIN32
#define strncasecmp strnicmp
#endif

extern void logText(char *s, ...);

#define MAX_NICK_LEN 128 // not including null term

#define TRANSFER_TIMEOUT 8

User_Connection::User_Connection(JNL_Connection *con, User_Group *grp) : m_auth_state(0), m_clientcaps(0), m_auth_privs(0), m_reserved(0)
{
  m_netcon.attach(con);

  WDL_RNG_bytes(m_challenge,sizeof(m_challenge));

  mpb_server_auth_challenge ch;
  memcpy(ch.challenge,m_challenge,sizeof(ch.challenge));

  ch.protocol_version = PROTO_VER_CUR;
  ch.server_caps=0;
  if (grp->m_licensetext.Get()[0])
    ch.license_agreement=grp->m_licensetext.Get();

  m_netcon.Send(ch.build());

  time(&m_connect_time);
}

User_Connection::~User_Connection()
{

  int x;
  for (x = 0; x < m_sublist.GetSize(); x ++)
    delete m_sublist.Get(x);
  m_sublist.Empty();
  for (x = 0; x < m_recvfiles.GetSize(); x ++)
    delete m_recvfiles.Get(x);
  m_recvfiles.Empty();
  for (x = 0; x < m_sendfiles.GetSize(); x ++)
    delete m_sendfiles.Get(x);
  m_sendfiles.Empty();
}


void User_Connection::SendConfigChangeNotify(int bpm, int bpi)
{
  if (m_auth_state > 0)
  {
    mpb_server_config_change_notify mk;
    mk.beats_interval=bpi;
    mk.beats_minute=bpm;
    m_netcon.Send(mk.build());
  }
}

int User_Connection::Run(User_Group *group, int *wantsleep)
{
  Net_Message *msg=m_netcon.Run(wantsleep);
  if (m_netcon.GetStatus()) 
  {
    delete msg;
    return m_netcon.GetStatus();
  }

  if (m_auth_state < 1 && !msg)
  {
     if (time(NULL) > m_connect_time+120) // if we haven't gotten an auth reply in 120s, disconnect. The reason this is so long is to give
                                        // the user time to potentially read the license agreement.
     {
        char buf[256];
        JNL::addr_to_ipstr(m_netcon.GetConnection()->get_remote(),buf,sizeof(buf));
        logText("%s: Got an authorization timeout\n",buf);
        m_connect_time=time(NULL)+120;
        mpb_server_auth_reply bh;
        bh.errmsg="authorization timeout";
        m_netcon.Send(bh.build());
        m_netcon.Run();

        m_netcon.Kill();
        return 0;
     }


  }
  if (msg)
  {
    msg->addRef();
    if (m_auth_state < 1)
    {
      mpb_client_auth_user authrep;
      if (msg->get_type() != MESSAGE_CLIENT_AUTH_USER || authrep.parse(msg) || !authrep.username || !authrep.username[0])
      {
        mpb_server_auth_reply bh;
        bh.errmsg="invalid authorization reply";
        m_netcon.Send(bh.build());
        m_netcon.Run();

        m_netcon.Kill();
        msg->releaseRef();
        return 0;
      }

      char addrbuf[256];
      JNL::addr_to_ipstr(m_netcon.GetConnection()->get_remote(),addrbuf,sizeof(addrbuf));

      char *username=authrep.username;
      char usernametmp[512];

      {
        char *anon=0;
        char authbuf[WDL_SHA1SIZE];
        unsigned int privs=0;
        
        if (group->GetUserPass && group->GetUserPass(group,username,authbuf,&anon,&privs))
        {
          if (authrep.client_version < PROTO_VER_MIN || authrep.client_version > PROTO_VER_MAX)
          {
            logText("%s: Refusing user %s, bad client version\n",addrbuf,username);
            mpb_server_auth_reply bh;
            bh.errmsg="incorrect client version";
            m_netcon.Send(bh.build());
            m_netcon.Run();

            m_netcon.Kill();
            msg->releaseRef();
            return 0;
          }
          else if (group->m_licensetext.Get()[0] && !(authrep.client_caps & 1)) // user didn't agree to license agreement
          {
            logText("%s: Refusing user %s, no license agreement\n",addrbuf,username);
            mpb_server_auth_reply bh;
            bh.errmsg="license not agreed to";
            m_netcon.Send(bh.build());
            m_netcon.Run();

            m_netcon.Kill();
            msg->releaseRef();
            return 0;
          }
          else if (anon)
          {
            if (*anon)
            {
              char pbuf[256];
              strncpy(pbuf,anon,255);
              pbuf[15]=0;

              char buf[128];
              JNL::addr_to_ipstr(m_netcon.GetConnection()->get_remote(),buf,sizeof(buf));
              sprintf(usernametmp+1,"%s-%s",pbuf,buf); // we make username = usernametmp+1 so we dont treat as a pure anonymous
              username=usernametmp+1;
            }
            else
            {
              JNL::addr_to_ipstr(m_netcon.GetConnection()->get_remote(),usernametmp,sizeof(usernametmp));
              username=usernametmp;
              strcat(usernametmp,"-");
            }
          }
          else
          {
            WDL_SHA1 shatmp;
            shatmp.add(authbuf,sizeof(authbuf));
            shatmp.add(m_challenge,sizeof(m_challenge));

            char buf[WDL_SHA1SIZE];
            shatmp.result(buf);
            if (memcmp(buf,authrep.passhash,WDL_SHA1SIZE))
            {
              logText("%s: Refusing user %s, invalid pass\n",addrbuf,username);
              mpb_server_auth_reply bh;
              bh.errmsg="invalid login/password";
              m_netcon.Send(bh.build());
              m_netcon.Run();
              m_netcon.Kill();
              msg->releaseRef();
              return 0;
            }

          }
        }
        else
        {
          logText("%s: Refusing user %s, invalid login\n",addrbuf,username);
          mpb_server_auth_reply bh;
          bh.errmsg="invalid login/password";
          m_netcon.Send(bh.build());
          m_netcon.Run();
          m_netcon.Kill();
          msg->releaseRef();
          return 0;
        }

        m_auth_privs=privs;

      }
      {
        // fix any invalid characters in username
        char *p=username;
        int l=MAX_NICK_LEN;
        while (*p)
        {
          char c=*p;
          if (!isalnum(c) && c != '-' && c != '_' && c != '@' && c != '.') c='_';
          *p++=c;

          if (!--l) *p=0;
        }
      }

      // disconnect any user by the same name
      // in anonymous mode, append -<idx>
      {
        int maxv=-1;
        int user;
        for (user = 0; user < group->m_users.GetSize(); user++)
        {
          User_Connection *u=group->m_users.Get(user);
          if (username == usernametmp)
          {
            if (u != this && !strncmp(u->m_username.Get(),username,strlen(username)))
            {
              int tv=atoi(u->m_username.Get()+strlen(username));
              if (tv > maxv) maxv=tv;
            }
          }
          else if (u != this && !strcasecmp(u->m_username.Get(),username))
          {
            delete u;
            group->m_users.Delete(user);
            break;
          }
        }

        if (username == usernametmp)
          sprintf(username+strlen(username),"%d",maxv+1);
      }


      if (group->m_max_users && !m_reserved && !(m_auth_privs & PRIV_RESERVE))
      {
        int user;
        int cnt=0;
        for (user = 0; user < group->m_users.GetSize(); user ++)
        {
          User_Connection *u=group->m_users.Get(user);
          if (u != this && u->m_auth_state > 0)
            cnt++;
        }
        if (cnt >= group->m_max_users)
        {
          logText("%s: Refusing user %s, server full\n",addrbuf,username);
          // sorry, gotta kill this connection
          mpb_server_auth_reply bh;
          bh.errmsg="server full";
          m_netcon.Send(bh.build());
          m_netcon.Run();
          m_netcon.Kill();
          msg->releaseRef();
          return 0;
        }
      }



      m_username.Set(username);
      logText("%s: Accepted user: %s\n",addrbuf,username);

      {
        mpb_server_auth_reply bh;
        bh.flag=1;
        bh.errmsg=m_username.Get();
        m_netcon.Send(bh.build());
      }

      m_clientcaps=authrep.client_caps;
      m_auth_state=1;

      SendConfigChangeNotify(group->m_last_bpm,group->m_last_bpi);

      // send user list to user
      {
        mpb_server_userinfo_change_notify bh;

        int user;
        for (user = 0; user < group->m_users.GetSize(); user++)
        {
          User_Connection *u=group->m_users.Get(user);
          int channel;
          if (u && u->m_auth_state>0) for (channel = 0; channel < MAX_USER_CHANNELS; channel ++)
          {
            if (u->m_channels[channel].active)
            {
              bh.build_add_rec(1,channel,u->m_channels[channel].volume,u->m_channels[channel].panning,u->m_channels[channel].flags,
                                u->m_username.Get(),u->m_channels[channel].name.Get());
            }
          }
        }       
        m_netcon.Send(bh.build());
      }


      {
        mpb_chat_message newmsg;
        newmsg.parms[0]="TOPIC";
        newmsg.parms[1]="";
        newmsg.parms[2]=group->m_topictext.Get();
        m_netcon.Send(newmsg.build());
      }
      {
        mpb_chat_message newmsg;
        newmsg.parms[0]="JOIN";
        newmsg.parms[1]=username;
        group->Broadcast(newmsg.build(),this);
      }


    } // m_auth_state < 1


    switch (msg->get_type())
    {
      case MESSAGE_CLIENT_SET_CHANNEL_INFO:
        {
          mpb_client_set_channel_info chi;
          if (!chi.parse(msg))
          {
            // update our channel list

            mpb_server_userinfo_change_notify mfmt;
            int mfmt_changes=0;
            
            int offs=0;
            short v;
            int p,f;
            int whichch=0;
            char *chnp=0;
            while ((offs=chi.parse_get_rec(offs,&chnp,&v,&p,&f))>0 && whichch < MAX_USER_CHANNELS)
            {
              if (!chnp) chnp=""; 

              // only if something changes, do we add it to the rec
              int hadch=!m_channels[whichch].active;
              if (!hadch) hadch = strcmp(chnp,m_channels[whichch].name.Get());
              if (!hadch) hadch = m_channels[whichch].volume!=v;
              if (!hadch) hadch = m_channels[whichch].panning!=p;
              if (!hadch) hadch = m_channels[whichch].flags!=f;

              m_channels[whichch].active=1;
              m_channels[whichch].name.Set(chnp);
              m_channels[whichch].volume=v;
              m_channels[whichch].panning=p;
              m_channels[whichch].flags=f;

              if (hadch)
              {
                mfmt_changes++;
                mfmt.build_add_rec(m_channels[whichch].active,whichch,
                                  m_channels[whichch].volume,
                                  m_channels[whichch].panning,
                                  m_channels[whichch].flags,
                                  m_username.Get(),
                                  m_channels[whichch].name.Get());
              }

              whichch++;
            }
            while (whichch < MAX_USER_CHANNELS)
            {
              m_channels[whichch].name.Set("");

              if (m_channels[whichch].active) // only send deactivate if it was previously active
              {
                m_channels[whichch].active=0;
                mfmt_changes++;
                mfmt.build_add_rec(0,whichch,
                                  m_channels[whichch].volume,
                                  m_channels[whichch].panning,
                                  m_channels[whichch].flags,
                                  m_username.Get(),
                                  m_channels[whichch].name.Get());
              }

              whichch++;
            }

            if (mfmt_changes) group->Broadcast(mfmt.build(),this);
          }         
        }
      break;
      case MESSAGE_CLIENT_SET_USERMASK:
        {
          mpb_client_set_usermask umi;
          if (!umi.parse(msg))
          {
            int offs=0;
            char *unp=0;
            unsigned int fla=0;
            while ((offs=umi.parse_get_rec(offs,&unp,&fla))>0)
            {
              if (unp)
              {
                int x;
                for (x = 0; x < m_sublist.GetSize() && strcasecmp(unp,m_sublist.Get(x)->username.Get()); x ++);
                if (x == m_sublist.GetSize()) // add new
                {
                  if (fla) // only add if we need to subscribe
                  {
                    User_SubscribeMask *n=new User_SubscribeMask;
                    n->username.Set(unp);
                    n->channelmask = fla;
                    m_sublist.Add(n);
                  }
                }
                else
                {
                  if (fla) // update flag
                  {
                    m_sublist.Get(x)->channelmask=fla;
                  }
                  else // remove
                  {
                    delete m_sublist.Get(x);
                    m_sublist.Delete(x);
                  }
                }
              }
            }
          }
        }
      break;
      case MESSAGE_CLIENT_UPLOAD_INTERVAL_BEGIN:
        {
          mpb_client_upload_interval_begin mp;
          if (!mp.parse(msg))
          {
            char *myusername=m_username.Get();

            mpb_server_download_interval_begin nmb;
            nmb.chidx=mp.chidx;
            nmb.estsize=mp.estsize;
            nmb.fourcc=mp.fourcc;
            memcpy(nmb.guid,mp.guid,sizeof(nmb.guid));
            nmb.username = myusername;

            Net_Message *newmsg=nmb.build();
            newmsg->addRef();
                      
            static unsigned char zero_guid[16];


            if (mp.fourcc && memcmp(mp.guid,zero_guid,sizeof(zero_guid))) // zero = silence, so simply rebroadcast
            {
              User_TransferState *newrecv=new User_TransferState;
              newrecv->bytes_estimated=mp.estsize;
              newrecv->fourcc=mp.fourcc;
              memcpy(newrecv->guid,mp.guid,sizeof(newrecv->guid));

              if (0) // server doesn't HAVE to cache, but might be useful eventually
              {
                char fn[512];
                int x;
                for (x = 0; x < 16; x ++)
                  wsprintf(fn+x*2,"%02x",mp.guid[x]);
                strcpy(fn+x*2,".ogg");

                newrecv->fp = fopen(fn,"wb");
              }
              
              m_recvfiles.Add(newrecv);
            }


            int user;
            for (user=0;user<group->m_users.GetSize(); user++)
            {
              User_Connection *u=group->m_users.Get(user);
              if (u && u != this)
              {
                int i;
                for (i=0; i < u->m_sublist.GetSize(); i ++)
                {
                  User_SubscribeMask *sm=u->m_sublist.Get(i);
                  if (!strcasecmp(sm->username.Get(),myusername))
                  {
                    if (sm->channelmask & (1<<mp.chidx))
                    {
                      if (memcmp(mp.guid,zero_guid,sizeof(zero_guid))) // zero = silence, so simply rebroadcast
                      {
                        // add entry in send list
                        User_TransferState *nt=new User_TransferState;
                        memcpy(nt->guid,mp.guid,sizeof(nt->guid));
                        nt->bytes_estimated = mp.estsize;
                        nt->fourcc = mp.fourcc;
                        u->m_sendfiles.Add(nt);
                      }

                      u->Send(newmsg);
                    }
                    break;
                  }
                }
              }
            }
            newmsg->releaseRef();
          }
        }
        //m_recvfiles
      break;
      case MESSAGE_CLIENT_UPLOAD_INTERVAL_WRITE:
        {
          mpb_client_upload_interval_write mp;
          if (!mp.parse(msg))
          {
            time_t now;
            time(&now);
            msg->set_type(MESSAGE_SERVER_DOWNLOAD_INTERVAL_WRITE); // we rely on the fact that the upload/download write messages are identical
                                                                   // though we may need to update this at a later date if we change things.

            int user,x;


            for (x = 0; x < m_recvfiles.GetSize(); x ++)
            {
              User_TransferState *t=m_recvfiles.Get(x);
              if (!memcmp(t->guid,mp.guid,sizeof(mp.guid)))
              {
                t->last_acttime=now;

                if (t->fp) fwrite(mp.audio_data,1,mp.audio_data_len,t->fp);

                t->bytes_sofar+=mp.audio_data_len;
                if (mp.flags & 1)
                {
                  delete t;
                  m_recvfiles.Delete(x);
                }
                break;
              }
              if (now-t->last_acttime > TRANSFER_TIMEOUT)
              {
                delete t;
                m_recvfiles.Delete(x--);
              }
            }


            for (user=0;user<group->m_users.GetSize(); user++)
            {
              User_Connection *u=group->m_users.Get(user);
              if (u && u != this)
              {
                int i;
                for (i=0; i < u->m_sendfiles.GetSize(); i ++)
                {
                  User_TransferState *t=u->m_sendfiles.Get(i);
                  if (t && !memcmp(t->guid,mp.guid,sizeof(t->guid)))
                  {
                    t->last_acttime=now;
                    t->bytes_sofar += mp.audio_data_len;
                    u->Send(msg);
                    if (mp.flags & 1)
                    {
                      delete t;
                      u->m_sendfiles.Delete(i);
                      // remove from transfer list
                    }
                    break;
                  }
                  if (now-t->last_acttime > TRANSFER_TIMEOUT)
                  {
                    delete t;
                    u->m_sendfiles.Delete(i--);
                  }
                }
              }
            }
          }
        }
      break;

      case MESSAGE_CHAT_MESSAGE:
        {
          mpb_chat_message poo;
          if (!poo.parse(msg))
          {
            group->onChatMessage(this,&poo);
          }
        }
      break;

      default:
      break;
    }
    msg->releaseRef();
  }


  return 0;
}


User_Group::User_Group() : m_max_users(0), m_last_bpm(120), m_last_bpi(32)
{
  GetUserPass=0;
}

User_Group::~User_Group()
{
  int x;
  for (x = 0; x < m_users.GetSize(); x ++)
  {
    delete m_users.Get(x);
  }
  m_users.Empty();
}


void User_Group::Broadcast(Net_Message *msg, User_Connection *nosend)
{
  if (msg)
  {
    msg->addRef(); // we do this so that if nobody actually gets to send it, we delete it

    int x;
    for (x = 0; x < m_users.GetSize(); x ++)
    {
      User_Connection *p=m_users.Get(x);
      if (p && p->m_auth_state > 0 && p != nosend)
      {
        p->Send(msg);
      }
    }

    msg->releaseRef();
  }
}

int User_Group::Run()
{
    int nosleep=0;
    int x;
    for (x = 0; x < m_users.GetSize(); x ++)
    {
      User_Connection *p=m_users.Get(x);
      if (p)
      {
        int nsl=1; // run will clear nsl if a sleep isnt desired
        if (p->Run(this,&nsl))
        {
          // broadcast to other users that this user is no longer present
          if (p->m_auth_state>0) 
          {
            mpb_chat_message newmsg;
            newmsg.parms[0]="PART";
            newmsg.parms[1]=p->m_username.Get();
            Broadcast(newmsg.build(),p);

            mpb_server_userinfo_change_notify mfmt;
            int mfmt_changes=0;

            int whichch=0;
            while (whichch < MAX_USER_CHANNELS)
            {
              p->m_channels[whichch].name.Set("");

              if (p->m_channels[whichch].active) // only send deactivate if it was previously active
              {
                p->m_channels[whichch].active=0;
                mfmt_changes++;
                mfmt.build_add_rec(0,whichch,
                                  p->m_channels[whichch].volume,
                                  p->m_channels[whichch].panning,
                                  p->m_channels[whichch].flags,
                                  p->m_username.Get(),
                                  p->m_channels[whichch].name.Get());
              }

              whichch++;
            }

            if (mfmt_changes) Broadcast(mfmt.build(),p);
          }

          delete p;
          m_users.Delete(x--);
        }
        if (!nsl) nosleep=1;
      }
    }
    return !nosleep;
}

void User_Group::SetConfig(int bpi, int bpm)
{
  m_last_bpi=bpi;
  m_last_bpm=bpm;
  mpb_server_config_change_notify mk;
  mk.beats_interval=bpi;
  mk.beats_minute=bpm;
  Broadcast(mk.build());
}

void User_Group::AddConnection(JNL_Connection *con, int isres)
{
  User_Connection *p=new User_Connection(con,this);
  if (isres) p->m_reserved=1;
  m_users.Add(p);
}

void User_Group::onChatMessage(User_Connection *con, mpb_chat_message *msg)
{
  if (!strcmp(msg->parms[0],"MSG")) // chat message
  {
    if (!(con->m_auth_privs & PRIV_CHATSEND))
    {
      mpb_chat_message newmsg;
      newmsg.parms[0]="MSG";
      newmsg.parms[1]="";
      newmsg.parms[2]="No MSG permission";
      con->Send(newmsg.build());
    }
    else if (msg->parms[1] && *msg->parms[1])
    {
      mpb_chat_message newmsg;
      newmsg.parms[0]="MSG";
      newmsg.parms[1]=con->m_username.Get();
      newmsg.parms[2]=msg->parms[1];
      Broadcast(newmsg.build());
    }
  }
  else if (!strcmp(msg->parms[0],"PRIVMSG")) // chat message
  {
    if (!(con->m_auth_privs & PRIV_CHATSEND))
    {
      mpb_chat_message newmsg;
      newmsg.parms[0]="MSG";
      newmsg.parms[1]="";
      newmsg.parms[2]="No PRIVMSG permission";
      con->Send(newmsg.build());
    }
    else if (msg->parms[1] && *msg->parms[1] && msg->parms[2] && *msg->parms[2])
    {
      // send a privmsg to user in parm1, and if they don't
      int x;
      for (x = 0; x < m_users.GetSize(); x ++)
      {
        if (!strcasecmp(msg->parms[1],m_users.Get(x)->m_username.Get()))
        {
          mpb_chat_message newmsg;
          newmsg.parms[0]="PRIVMSG";
          newmsg.parms[1]=con->m_username.Get();
          newmsg.parms[2]=msg->parms[2];
          m_users.Get(x)->Send(newmsg.build());

          return;
        }
      }

      // send a privmsg back to sender, saying shit aint there
      WDL_String buf("No such user: ");
      buf.Append(msg->parms[1]);          
      mpb_chat_message newmsg;
      newmsg.parms[0]="MSG";
      newmsg.parms[1]="";
      newmsg.parms[2]=buf.Get();
      con->Send(newmsg.build());
    }
  }
  else if (!strcmp(msg->parms[0],"ADMIN")) // admin message
  {
    char *adminerr="ADMIN requires valid parameter, i.e. topic, kick, bpm, bpi";
    if (msg->parms[1] && *msg->parms[1])
    {
      if (!strncasecmp(msg->parms[1],"topic ",6))
      {
        if (!(con->m_auth_privs & PRIV_TOPIC))
        {
          mpb_chat_message newmsg;
          newmsg.parms[0]="MSG";
          newmsg.parms[1]="";
          newmsg.parms[2]="No TOPIC permission";
          con->Send(newmsg.build());
        }
        else
        {
          // set topic, notify everybody of topic change
          char *p=msg->parms[1]+6;
          while (*p == ' ') p++;
          if (*p)
          {
            m_topictext.Set(p);
            mpb_chat_message newmsg;
            newmsg.parms[0]="TOPIC";
            newmsg.parms[1]=con->m_username.Get();
            newmsg.parms[2]=m_topictext.Get();
            Broadcast(newmsg.build());
          }
        }

      }
      else if (!strncasecmp(msg->parms[1],"kick ",5))
      {
        if (!(con->m_auth_privs & PRIV_KICK))
        {
          mpb_chat_message newmsg;
          newmsg.parms[0]="MSG";
          newmsg.parms[1]="";
          newmsg.parms[2]="No KICK permission";
          con->Send(newmsg.build());
        }
        else
        {
          // set topic, notify everybody of topic change
          char *p=msg->parms[1]+5;
          while (*p == ' ') p++;
          if (*p)
          {
            // try to kick user
            int x;
            int killcnt=0;
            int pl=strlen(p);
            for (x = 0; x < m_users.GetSize(); x ++)
            {
              User_Connection *c=m_users.Get(x);
              if ((p[pl-1] == '*' && !strncasecmp(c->m_username.Get(),p,pl-1)) || !strcasecmp(c->m_username.Get(),p))
              {
                if (c != con)
                {
                  WDL_String buf("User ");
                  buf.Append(c->m_username.Get());
                  buf.Append(" kicked by ");
                  buf.Append(con->m_username.Get());

                  mpb_chat_message newmsg;
                  newmsg.parms[0]="MSG";
                  newmsg.parms[1]="";
                  newmsg.parms[2]=buf.Get();
                  Broadcast(newmsg.build());

                  c->m_netcon.Kill();
                }
                killcnt++;
              }
            }
            if (!killcnt)
            {
              WDL_String tmp;
              tmp.Set("User \"");
              tmp.Append(p);
              tmp.Append("\" not found!\n");

              mpb_chat_message newmsg;
              newmsg.parms[0]="MSG";
              newmsg.parms[1]="";
              newmsg.parms[2]=tmp.Get();
              con->Send(newmsg.build());
            }

          }
        }

      }
      else if (!strncasecmp(msg->parms[1],"bpm ",4) || !strncasecmp(msg->parms[1],"bpi ",4))
      {
        if (!(con->m_auth_privs & PRIV_BPM))
        {
          mpb_chat_message newmsg;
          newmsg.parms[0]="MSG";
          newmsg.parms[1]="";
          newmsg.parms[2]="No BPM/BPI permission";
          con->Send(newmsg.build());
        }
        else
        {
          int isbpm=tolower(msg->parms[1][2])=='m';

          char *p=msg->parms[1]+4;
          while (*p == ' ') p++;
          int v=atoi(p);
          if (isbpm && (v < 20 || v > 400))
          {
            mpb_chat_message newmsg;
            newmsg.parms[0]="MSG";
            newmsg.parms[1]="";
            newmsg.parms[2]="BPM parameter must be between 20 and 400";
            con->Send(newmsg.build());
          }
          else if (!isbpm && (v < 2 || v > 1024))
          {
            mpb_chat_message newmsg;
            newmsg.parms[0]="MSG";
            newmsg.parms[1]="";
            newmsg.parms[2]="BPI parameter must be between 2 and 1024";
            con->Send(newmsg.build());
          }
          else
          {
            if (isbpm) m_last_bpm=v;
            else m_last_bpi=v;

            mpb_server_config_change_notify mk;
            mk.beats_interval=m_last_bpi;
            mk.beats_minute=m_last_bpm;
            Broadcast(mk.build());

            WDL_String str(con->m_username.Get());
            str.Append(" sets ");
            if (isbpm) str.Append("BPM"); else str.Append("BPI");
            str.Append(" to ");
            char buf[64];
            sprintf(buf,"%d",v);
            str.Append(buf);

            mpb_chat_message newmsg;
            newmsg.parms[0]="MSG";
            newmsg.parms[1]="";
            newmsg.parms[2]=str.Get();
            Broadcast(newmsg.build());
          }
        }

      }
      else
      {
        mpb_chat_message newmsg;
        newmsg.parms[0]="MSG";
        newmsg.parms[1]="";
        newmsg.parms[2]=adminerr;
        con->Send(newmsg.build());
      }
    }
    else
    {
      mpb_chat_message newmsg;
      newmsg.parms[0]="MSG";
      newmsg.parms[1]="";
      newmsg.parms[2]=adminerr;
      con->Send(newmsg.build());
    }
  }
  else // unknown message
  {
  }
}
