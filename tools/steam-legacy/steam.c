/* A minimal steam.dll for games from before Steamworks (the Steam2 client library).
 *
 * Those games load steam.dll at start and stop unless it says Steam is running and the account
 * owns the game: GUN's wrapper calls SteamStartup, SteamIsAppSubscribed and SteamCleanup and
 * exits with "Failed to find Steam." when the DLL is missing. On the console there is no Steam
 * client, and Z3CloudPlay already checked ownership when it downloaded the game, so every call here
 * succeeds. All exports are cdecl with undecorated names, as Valve's steam.dll has them.
 *
 * Build (32-bit; Steam2-era games are all 32-bit): tools/steam-legacy/build.sh
 */
typedef struct {
  int eSteamError;
  int eDetailedErrorType;
  int nDetailedErrorCode;
  char szDesc[255];
} TSteamError;

/* No C runtime: the DLL imports nothing. */
static void Clear(TSteamError *error) {
  unsigned char *bytes = (unsigned char *)error;
  unsigned int i;
  if (!error) return;
  for (i = 0; i < sizeof(*error); ++i) bytes[i] = 0;
}

__declspec(dllexport) int SteamStartup(unsigned int interfaces, TSteamError *error) {
  (void)interfaces;
  Clear(error);
  return 1;
}

__declspec(dllexport) int SteamCleanup(TSteamError *error) {
  Clear(error);
  return 1;
}

__declspec(dllexport) int SteamIsAppSubscribed(unsigned int app, int *subscribed, int *pending, TSteamError *error) {
  (void)app;
  if (subscribed) *subscribed = 1;
  if (pending) *pending = 0;
  Clear(error);
  return 1;
}

__declspec(dllexport) int SteamIsSubscribed(unsigned int subscription, int *subscribed, int *pending, TSteamError *error) {
  (void)subscription;
  if (subscribed) *subscribed = 1;
  if (pending) *pending = 0;
  Clear(error);
  return 1;
}

__declspec(dllexport) int SteamIsLoggedIn(int *logged_in, TSteamError *error) {
  if (logged_in) *logged_in = 1;
  Clear(error);
  return 1;
}

__declspec(dllexport) int SteamGetLocalClientVersion(unsigned int *version, unsigned int *source, TSteamError *error) {
  if (version) *version = 0;
  if (source) *source = 0;
  Clear(error);
  return 1;
}

int __stdcall DllMain(void *instance, unsigned long reason, void *reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return 1;
}
