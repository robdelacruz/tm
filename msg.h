#ifndef MSG_H
#define MSG_H

#define REGISTERUSER_REQUEST 1
#define REGISTERUSER_RESPONSE 2
#define LOGINUSER_REQUEST 3
#define LOGINUSER_RESPONSE 4
#define GETCONTACTS_REQUEST 5
#define GETCONTACTS_RESPONSE 6
#define GETONLINECONTACTS_REQUEST 5
#define GETONLINECONTACTS_RESPONSE 6
#define SEARCHUSERNAME_REQUEST 7
#define SEARCHUSERNAME_RESPONSE 8
#define INVITECONTACT_REQUEST 9
#define INVITECONTACT_RESPONSE 10
#define GETINVITES_REQUEST 11
#define GETINVITES_RESPONSE 12
#define APPROVECONTACTS_REQUEST 13
#define APPROVECONTACTS_RESPONSE 14

#define MSGNO(bs) (*((u8 *)bs))

// REGISTERUSER_REQUEST
typedef struct {
    u8 msgno;
    String username;
    String pwd;
} RegisterUserRequest;

// REGISTERUSER_RESPONSE
typedef struct {
    u8 msgno;
    String tok;
    String username;
    u8 retno;
    String errortext;
} RegisterUserResponse;

// LOGINUSER_REQUEST
typedef struct {
    u8 msgno;
    String username;
    String pwd;
} LoginUserRequest;

// LOGINUSER_RESPONSE
typedef struct {
    u8 msgno;
    String tok;
    String username;
    u8 retno;
    String errortext;
} LoginUserResponse;

// GETCONTACTS_REQUEST
typedef struct {
    u8 msgno;
    String tok;
} GetContactsRequest;

// GETCONTACTS_RESPONSE
typedef struct {
    u8 msgno;
    String usernames;
} ContactsResponse;

// GETONLINECONTACTS_REQUEST
typedef struct {
    u8 msgno;
    String tok;
} GetOnlineContactsRequest;

// GETCONTACTS_RESPONSE
typedef struct {
    u8 msgno;
    String usernames;
} OnlineContactsResponse;

// SEARCHUSERNAME_REQUEST
typedef struct {
    u8 msgno;
    String tok;
    String searchtext;
} SearchUsernameRequest;

// SEARCHUSERNAME_RESPONSE
typedef struct {
    u8 msgno;
    String usernames;
} SearchUsernameResponse;

// INVITECONTACT_REQUEST
typedef struct {
    u8 msgno;
    String tok;
    String invite_username;
    String message;
} InviteContactRequest;

// INVITECONTACT_RESPONSE
typedef struct {
    u8 msgno;
    u8 retno;
    String errortext;
} InviteContactResponse;

// GETINVITES_REQUEST
typedef struct {
    u8 msgno;
    String tok;
} GetInvitesRequest;

// GETINVITES_RESPONSE
typedef struct {
    u8 msgno;
    u8 num_invites;
    String usernames;
    String messages;
} GetInvitesResponse;

// APPROVECONTACTS_REQUEST
typedef struct {
    u8 msgno;
    String tok;
    String approved_usernames;
} ApproveInvitesRequest;

// APPROVECONTACTS_RESPONSE
typedef struct {
    u8 msgno;
    u8 retno;
    String errortext;
} ApproveInvitesResponse;

#endif
