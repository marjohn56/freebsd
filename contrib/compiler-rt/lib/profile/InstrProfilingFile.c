/*===- InstrProfilingFile.c - Write instrumentation to a file -------------===*\
|*
|*                     The LLVM Compiler Infrastructure
|*
|* This file is distributed under the University of Illinois Open Source
|* License. See LICENSE.TXT for details.
|*
\*===----------------------------------------------------------------------===*/

#include "InstrProfiling.h"
#include "InstrProfilingInternal.h"
#include "InstrProfilingUtil.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
/* For _alloca. */
#include <malloc.h>
#endif
#if defined(_WIN32)
#include "WindowsMMap.h"
/* For _chsize_s */
#include <io.h>
#else
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/types.h>
#endif
#endif

/* From where is profile name specified.
 * The order the enumerators define their
 * precedence. Re-order them may lead to
 * runtime behavior change. */ 
typedef enum ProfileNameSpecifier {
  PNS_unknown = 0,
  PNS_default,
  PNS_command_line,
  PNS_environment,
  PNS_runtime_api
} ProfileNameSpecifier;

static const char *getPNSStr(ProfileNameSpecifier PNS) {
  switch (PNS) {
  case PNS_default:
    return "default setting";
  case PNS_command_line:
    return "command line";
  case PNS_environment:
    return "environment variable";
  case PNS_runtime_api:
    return "runtime API";
  default:
    return "Unknown";
  }
}

#define MAX_PID_SIZE 16
/* Data structure holding the result of parsed filename pattern.  */
typedef struct lprofFilename {
  /* File name string possibly with %p or %h specifiers. */
  const char *FilenamePat;
  char PidChars[MAX_PID_SIZE];
  char Hostname[COMPILER_RT_MAX_HOSTLEN];
  unsigned NumPids;
  unsigned NumHosts;
  /* When in-process merging is enabled, this parameter specifies
   * the total number of profile data files shared by all the processes
   * spawned from the same binary. By default the value is 1. If merging
   * is not enabled, its value should be 0. This parameter is specified
   * by the %[0-9]m specifier. For instance %2m enables merging using
   * 2 profile data files. %1m is equivalent to %m. Also %m specifier
   * can only appear once at the end of the name pattern. */
  unsigned MergePoolSize;
  ProfileNameSpecifier PNS;
} lprofFilename;

lprofFilename lprofCurFilename = {0, {0}, {0}, 0, 0, 0, PNS_unknown};

int getpid(void);
static int getCurFilenameLength();
static const char *getCurFilename(char *FilenameBuf);
static unsigned doMerging() { return lprofCurFilename.MergePoolSize; }

/* Return 1 if there is an error, otherwise return  0.  */
static uint32_t fileWriter(ProfDataIOVec *IOVecs, uint32_t NumIOVecs,
                           void **WriterCtx) {
  uint32_t I;
  FILE *File = (FILE *)*WriterCtx;
  for (I = 0; I < NumIOVecs; I++) {
    if (fwrite(IOVecs[I].Data, IOVecs[I].ElmSize, IOVecs[I].NumElm, File) !=
        IOVecs[I].NumElm)
      return 1;
  }
  return 0;
}

COMPILER_RT_VISIBILITY ProfBufferIO *
lprofCreateBufferIOInternal(void *File, uint32_t BufferSz) {
  FreeHook = &free;
  DynamicBufferIOBuffer = (uint8_t *)calloc(BufferSz, 1);
  VPBufferSize = BufferSz;
  return lprofCreateBufferIO(fileWriter, File);
}

static void setupIOBuffer() {
  const char *BufferSzStr = 0;
  BufferSzStr = getenv("LLVM_VP_BUFFER_SIZE");
  if (BufferSzStr && BufferSzStr[0]) {
    VPBufferSize = atoi(BufferSzStr);
    DynamicBufferIOBuffer = (uint8_t *)calloc(VPBufferSize, 1);
  }
}

/* Read profile data in \c ProfileFile and merge with in-memory
   profile counters. Returns -1 if there is fatal error, otheriwse
   0 is returned.
*/
static int doProfileMerging(FILE *ProfileFile) {
  uint64_t ProfileFileSize;
  char *ProfileBuffer;

  if (fseek(ProfileFile, 0L, SEEK_END) == -1) {
    PROF_ERR("Unable to merge profile data, unable to get size: %s\n",
             strerror(errno));
    return -1;
  }
  ProfileFileSize = ftell(ProfileFile);

  /* Restore file offset.  */
  if (fseek(ProfileFile, 0L, SEEK_SET) == -1) {
    PROF_ERR("Unable to merge profile data, unable to rewind: %s\n",
             strerror(errno));
    return -1;
  }

  /* Nothing to merge.  */
  if (ProfileFileSize < sizeof(__llvm_profile_header)) {
    if (ProfileFileSize)
      PROF_WARN("Unable to merge profile data: %s\n",
                "source profile file is too small.");
    return 0;
  }

  ProfileBuffer = mmap(NULL, ProfileFileSize, PROT_READ, MAP_SHARED | MAP_FILE,
                       fileno(ProfileFile), 0);
  if (ProfileBuffer == MAP_FAILED) {
    PROF_ERR("Unable to merge profile data, mmap failed: %s\n",
             strerror(errno));
    return -1;
  }

  if (__llvm_profile_check_compatibility(ProfileBuffer, ProfileFileSize)) {
    (void)munmap(ProfileBuffer, ProfileFileSize);
    PROF_WARN("Unable to merge profile data: %s\n",
              "source profile file is not compatible.");
    return 0;
  }

  /* Now start merging */
  __llvm_profile_merge_from_buffer(ProfileBuffer, ProfileFileSize);
  (void)munmap(ProfileBuffer, ProfileFileSize);

  return 0;
}

/* Open the profile data for merging. It opens the file in r+b mode with
 * file locking.  If the file has content which is compatible with the
 * current process, it also reads in the profile data in the file and merge
 * it with in-memory counters. After the profile data is merged in memory,
 * the original profile data is truncated and gets ready for the profile
 * dumper. With profile merging enabled, each executable as well as any of
 * its instrumented shared libraries dump profile data into their own data file.
*/
static FILE *openFileForMerging(const char *ProfileFileName) {
  FILE *ProfileFile;
  int rc;

  ProfileFile = lprofOpenFileEx(ProfileFileName);
  if (!ProfileFile)
    return NULL;

  rc = doProfileMerging(ProfileFile);
  if (rc || COMPILER_RT_FTRUNCATE(ProfileFile, 0L) ||
      fseek(ProfileFile, 0L, SEEK_SET) == -1) {
    PROF_ERR("Profile Merging of file %s failed: %s\n", ProfileFileName,
             strerror(errno));
    fclose(ProfileFile);
    return NULL;
  }
  fseek(ProfileFile, 0L, SEEK_SET);
  return ProfileFile;
}

/* Write profile data to file \c OutputName.  */
static int writeFile(const char *OutputName) {
  int RetVal;
  FILE *OutputFile;

  if (!doMerging())
    OutputFile = fopen(OutputName, "ab");
  else
    OutputFile = openFileForMerging(OutputName);

  if (!OutputFile)
    return -1;

  FreeHook = &free;
  setupIOBuffer();
  RetVal = lprofWriteData(fileWriter, OutputFile, lprofGetVPDataReader());

  fclose(OutputFile);
  return RetVal;
}

static void truncateCurrentFile(void) {
  const char *Filename;
  char *FilenameBuf;
  FILE *File;
  int Length;

  Length = getCurFilenameLength();
  FilenameBuf = (char *)COMPILER_RT_ALLOCA(Length + 1);
  Filename = getCurFilename(FilenameBuf);
  if (!Filename)
    return;

  /* Create the directory holding the file, if needed. */
  if (strchr(Filename, DIR_SEPARATOR)
#if defined(DIR_SEPARATOR_2)
      || strchr(Filename, DIR_SEPARATOR_2)
#endif
          ) {
    char *Copy = (char *)COMPILER_RT_ALLOCA(Length + 1);
    strncpy(Copy, Filename, Length + 1);
    __llvm_profile_recursive_mkdir(Copy);
  }

  /* Truncate the file.  Later we'll reopen and append. */
  File = fopen(Filename, "w");
  if (!File)
    return;
  fclose(File);
}

static const char *DefaultProfileName = "default.profraw";
static void resetFilenameToDefault(void) {
  memset(&lprofCurFilename, 0, sizeof(lprofCurFilename));
  lprofCurFilename.FilenamePat = DefaultProfileName;
  lprofCurFilename.PNS = PNS_default;
}

static int containsMergeSpecifier(const char *FilenamePat, int I) {
  return (FilenamePat[I] == 'm' ||
          (FilenamePat[I] >= '1' && FilenamePat[I] <= '9' &&
           /* If FilenamePat[I] is not '\0', the next byte is guaranteed
            * to be in-bound as the string is null terminated. */
           FilenamePat[I + 1] == 'm'));
}

/* Parses the pattern string \p FilenamePat and stores the result to
 * lprofcurFilename structure. */
static int parseFilenamePattern(const char *FilenamePat) {
  int NumPids = 0, NumHosts = 0, I;
  char *PidChars = &lprofCurFilename.PidChars[0];
  char *Hostname = &lprofCurFilename.Hostname[0];
  int MergingEnabled = 0;

  lprofCurFilename.FilenamePat = FilenamePat;
  /* Check the filename for "%p", which indicates a pid-substitution. */
  for (I = 0; FilenamePat[I]; ++I)
    if (FilenamePat[I] == '%') {
      if (FilenamePat[++I] == 'p') {
        if (!NumPids++) {
          if (snprintf(PidChars, MAX_PID_SIZE, "%d", getpid()) <= 0) {
            PROF_WARN(
                "Unable to parse filename pattern %s. Using the default name.",
                FilenamePat);
            return -1;
          }
        }
      } else if (FilenamePat[I] == 'h') {
        if (!NumHosts++)
          if (COMPILER_RT_GETHOSTNAME(Hostname, COMPILER_RT_MAX_HOSTLEN)) {
            PROF_WARN(
                "Unable to parse filename pattern %s. Using the default name.",
                FilenamePat);
            return -1;
          }
      } else if (containsMergeSpecifier(FilenamePat, I)) {
        if (MergingEnabled) {
          PROF_WARN("%%m specifier can only be specified once in %s.\n",
                    FilenamePat);
          return -1;
        }
        MergingEnabled = 1;
        if (FilenamePat[I] == 'm')
          lprofCurFilename.MergePoolSize = 1;
        else {
          lprofCurFilename.MergePoolSize = FilenamePat[I] - '0';
          I++; /* advance to 'm' */
        }
      }
    }

  lprofCurFilename.NumPids = NumPids;
  lprofCurFilename.NumHosts = NumHosts;
  return 0;
}

static void parseAndSetFilename(const char *FilenamePat,
                                ProfileNameSpecifier PNS) {

  const char *OldFilenamePat = lprofCurFilename.FilenamePat;
  ProfileNameSpecifier OldPNS = lprofCurFilename.PNS;

  if (PNS < OldPNS)
    return;

  if (!FilenamePat)
    FilenamePat = DefaultProfileName;

  /* When -fprofile-instr-generate=<path> is specified on the
   * command line, each module will be instrumented with runtime
   * init call to __llvm_profile_init function which calls
   * __llvm_profile_override_default_filename. In most of the cases,
   * the path will be identical, so bypass the parsing completely.
   */
  if (OldFilenamePat && !strcmp(OldFilenamePat, FilenamePat)) {
    lprofCurFilename.PNS = PNS;
    return;
  }

  /* When PNS >= OldPNS, the last one wins. */
  if (!FilenamePat || parseFilenamePattern(FilenamePat))
    resetFilenameToDefault();
  lprofCurFilename.PNS = PNS;

  if (!OldFilenamePat) {
    PROF_NOTE("Set profile file path to \"%s\" via %s.\n",
              lprofCurFilename.FilenamePat, getPNSStr(PNS));
  } else {
    PROF_NOTE("Override old profile path \"%s\" via %s to \"%s\" via %s.\n",
              OldFilenamePat, getPNSStr(OldPNS), lprofCurFilename.FilenamePat,
              getPNSStr(PNS));
  }

  if (!lprofCurFilename.MergePoolSize)
    truncateCurrentFile();
}

/* Return buffer length that is required to store the current profile
 * filename with PID and hostname substitutions. */
/* The length to hold uint64_t followed by 2 digit pool id including '_' */
#define SIGLEN 24
static int getCurFilenameLength() {
  int Len;
  if (!lprofCurFilename.FilenamePat || !lprofCurFilename.FilenamePat[0])
    return 0;

  if (!(lprofCurFilename.NumPids || lprofCurFilename.NumHosts ||
        lprofCurFilename.MergePoolSize))
    return strlen(lprofCurFilename.FilenamePat);

  Len = strlen(lprofCurFilename.FilenamePat) +
        lprofCurFilename.NumPids * (strlen(lprofCurFilename.PidChars) - 2) +
        lprofCurFilename.NumHosts * (strlen(lprofCurFilename.Hostname) - 2);
  if (lprofCurFilename.MergePoolSize)
    Len += SIGLEN;
  return Len;
}

/* Return the pointer to the current profile file name (after substituting
 * PIDs and Hostnames in filename pattern. \p FilenameBuf is the buffer
 * to store the resulting filename. If no substitution is needed, the
 * current filename pattern string is directly returned. */
static const char *getCurFilename(char *FilenameBuf) {
  int I, J, PidLength, HostNameLength;
  const char *FilenamePat = lprofCurFilename.FilenamePat;

  if (!lprofCurFilename.FilenamePat || !lprofCurFilename.FilenamePat[0])
    return 0;

  if (!(lprofCurFilename.NumPids || lprofCurFilename.NumHosts ||
        lprofCurFilename.MergePoolSize))
    return lprofCurFilename.FilenamePat;

  PidLength = strlen(lprofCurFilename.PidChars);
  HostNameLength = strlen(lprofCurFilename.Hostname);
  /* Construct the new filename. */
  for (I = 0, J = 0; FilenamePat[I]; ++I)
    if (FilenamePat[I] == '%') {
      if (FilenamePat[++I] == 'p') {
        memcpy(FilenameBuf + J, lprofCurFilename.PidChars, PidLength);
        J += PidLength;
      } else if (FilenamePat[I] == 'h') {
        memcpy(FilenameBuf + J, lprofCurFilename.Hostname, HostNameLength);
        J += HostNameLength;
      } else if (containsMergeSpecifier(FilenamePat, I)) {
        char LoadModuleSignature[SIGLEN];
        int S;
        int ProfilePoolId = getpid() % lprofCurFilename.MergePoolSize;
        S = snprintf(LoadModuleSignature, SIGLEN, "%" PRIu64 "_%d",
                     lprofGetLoadModuleSignature(), ProfilePoolId);
        if (S == -1 || S > SIGLEN)
          S = SIGLEN;
        memcpy(FilenameBuf + J, LoadModuleSignature, S);
        J += S;
        if (FilenamePat[I] != 'm')
          I++;
      }
      /* Drop any unknown substitutions. */
    } else
      FilenameBuf[J++] = FilenamePat[I];
  FilenameBuf[J] = 0;

  return FilenameBuf;
}

/* Returns the pointer to the environment variable
 * string. Returns null if the env var is not set. */
static const char *getFilenamePatFromEnv(void) {
  const char *Filename = getenv("LLVM_PROFILE_FILE");
  if (!Filename || !Filename[0])
    return 0;
  return Filename;
}

/* This method is invoked by the runtime initialization hook
 * InstrProfilingRuntime.o if it is linked in. Both user specified
 * profile path via -fprofile-instr-generate= and LLVM_PROFILE_FILE
 * environment variable can override this default value. */
COMPILER_RT_VISIBILITY
void __llvm_profile_initialize_file(void) {
  const char *FilenamePat;

  FilenamePat = getFilenamePatFromEnv();
  parseAndSetFilename(FilenamePat, FilenamePat ? PNS_environment : PNS_default);
}

/* This API is directly called by the user application code. It has the
 * highest precedence compared with LLVM_PROFILE_FILE environment variable
 * and command line option -fprofile-instr-generate=<profile_name>.
 */
COMPILER_RT_VISIBILITY
void __llvm_profile_set_filename(const char *FilenamePat) {
  parseAndSetFilename(FilenamePat, PNS_runtime_api);
}

/*
 * This API is invoked by the global initializers emitted by Clang/LLVM when
 * -fprofile-instr-generate=<..> is specified (vs -fprofile-instr-generate
 *  without an argument). This option has lower precedence than the
 *  LLVM_PROFILE_FILE environment variable.
 */
COMPILER_RT_VISIBILITY
void __llvm_profile_override_default_filename(const char *FilenamePat) {
  parseAndSetFilename(FilenamePat, PNS_command_line);
}

/* The public API for writing profile data into the file with name
 * set by previous calls to __llvm_profile_set_filename or
 * __llvm_profile_override_default_filename or
 * __llvm_profile_initialize_file. */
COMPILER_RT_VISIBILITY
int __llvm_profile_write_file(void) {
  int rc, Length;
  const char *Filename;
  char *FilenameBuf;

  Length = getCurFilenameLength();
  FilenameBuf = (char *)COMPILER_RT_ALLOCA(Length + 1);
  Filename = getCurFilename(FilenameBuf);

  /* Check the filename. */
  if (!Filename) {
    PROF_ERR("Failed to write file : %s\n", "Filename not set");
    return -1;
  }

  /* Check if there is llvm/runtime version mismatch.  */
  if (GET_VERSION(__llvm_profile_get_version()) != INSTR_PROF_RAW_VERSION) {
    PROF_ERR("Runtime and instrumentation version mismatch : "
             "expected %d, but get %d\n",
             INSTR_PROF_RAW_VERSION,
             (int)GET_VERSION(__llvm_profile_get_version()));
    return -1;
  }

  /* Write profile data to the file. */
  rc = writeFile(Filename);
  if (rc)
    PROF_ERR("Failed to write file \"%s\": %s\n", Filename, strerror(errno));
  return rc;
}

static void writeFileWithoutReturn(void) { __llvm_profile_write_file(); }

COMPILER_RT_VISIBILITY
int __llvm_profile_register_write_file_atexit(void) {
  static int HasBeenRegistered = 0;

  if (HasBeenRegistered)
    return 0;

  lprofSetupValueProfiler();

  HasBeenRegistered = 1;
  return atexit(writeFileWithoutReturn);
}
