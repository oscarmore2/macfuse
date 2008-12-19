// sshfs.app
// Copyright 2007, Google Inc.
//
// Redistribution and use in source and binary forms, with or without 
// modification, are permitted provided that the following conditions are met:
//
//  1. Redistributions of source code must retain the above copyright notice, 
//     this list of conditions and the following disclaimer.
//  2. Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//  3. The name of the author may not be used to endorse or promote products 
//     derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
// EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR 
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF 
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <unistd.h>
#include <Security/Security.h>
#include <SecurityFoundation/SFAuthorization.h>

#import "DiskImageUtilities.h"

static NSString* const kHDIUtilPath = @"/usr/bin/hdiutil";

@interface DiskImageUtilities (PrivateMethods_handleApplicationLaunchFromReadOnlyDiskImage)
+ (BOOL)canWriteToPath:(NSString *)path;
+ (void)copyAndLaunchAppAtPath:(NSString *)oldPath
               toDirectoryPath:(NSString *)newDirectory;
+ (void)killAppAtPath:(NSString *)appPath;
+ (BOOL)doAuthorizedCopyFromPath:(NSString *)src toPath:(NSString *)dest;
@end

@implementation DiskImageUtilities

// get the mounted disk image info as a dictionary
+ (NSDictionary *)diskImageInfo {
  
  NSDictionary *resultDict = nil;
  
  NSArray* args = [NSArray arrayWithObjects:@"info", @"-plist", nil];
  // -plist means the results will be written as a property list to stdout
  
  NSPipe* outputPipe = [NSPipe pipe];
  NSTask* theTask = [[[NSTask alloc] init] autorelease];
  [theTask setLaunchPath:kHDIUtilPath];
  [theTask setArguments:args];
  [theTask setStandardOutput:outputPipe];
  
  [theTask launch];
  
  NSFileHandle *outputFile = [outputPipe fileHandleForReading];
  NSData *plistData = nil;
  @try {
    plistData = [outputFile readDataToEndOfFile]; // blocks until EOF delivered
  }
  @catch(id obj) {
    // running in gdb we get exception: Interrupted system call
    NSLog(@"DiskImageUtilities diskImageInfo: gdb issue -- "
                "getting file data causes exception: %@", obj);
  }
  [theTask waitUntilExit];
  int status = [theTask terminationStatus];
  
  if (status != 0 || [plistData length] == 0) {
    
    NSLog(@"DiskImageUtilities diskImageInfo: hdiutil failed, result %d", status); 
    
  } else {
    NSString *plist = [[[NSString alloc] initWithData:plistData 
                                             encoding:NSUTF8StringEncoding] autorelease];
    resultDict = [plist propertyList];
  }    
  return resultDict;
}

+ (NSArray *)readOnlyDiskImagePaths {
  
  NSMutableArray *paths = [NSMutableArray array];
  NSDictionary *dict = [self diskImageInfo];
  if (dict) {
    
    NSArray *imagesArray = [dict objectForKey:@"images"];
    
    // we have an array of dicts for the known images
    //
    // we want to find non-writable images, and get the mount
    // points from their system entities
    
    if (imagesArray) {
      int idx;
      unsigned int numberOfImages = [imagesArray count];
      for (idx = 0; idx < numberOfImages; idx++) {
        
        NSDictionary *imageDict = [imagesArray objectAtIndex:idx];
        NSNumber *isWriteable = [imageDict objectForKey:@"writeable"];
        if (isWriteable && ![isWriteable boolValue]) {
          
          NSArray *systemEntitiesArray = [imageDict objectForKey:@"system-entities"];
          if (systemEntitiesArray) {
            int idx;
            unsigned int numberOfSystemEntities = [systemEntitiesArray count];
            for (idx = 0; idx < numberOfSystemEntities; idx++) {
              
              NSDictionary *entityDict = [systemEntitiesArray objectAtIndex:idx];
              NSString *mountPoint = [entityDict objectForKey:@"mount-point"];
              if ([mountPoint length] > 0) {
                
                // found a read-only image mount point; add it to our list
                // and move to the next image
                [paths addObject:mountPoint];
                break;
              }
            }
          }
        }
      }
    }
  }
  return paths;
}

// checks if the current app is running from a disk image,
// displays a dialog offering to copy to /Applications, and
// does the copying

+ (void)handleApplicationLaunchFromReadOnlyDiskImage {
  
  NSString * const kLastLaunchedPathKey = @"LastLaunchedPath";
  NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
  
  NSString *lastLaunchedPath = [defaults objectForKey:kLastLaunchedPathKey];
  NSString *mainBundlePath = [[NSBundle mainBundle] bundlePath];
  
  BOOL isRunningFromDiskImage = NO;
  
  if (lastLaunchedPath == nil 
      || ![lastLaunchedPath isEqualToString:mainBundlePath]) {
    
    // we haven't tested this launch path; check it now
    NSArray *imagePaths = [self readOnlyDiskImagePaths];
    
    int idx;
    for (idx = 0; idx < [imagePaths count]; idx++) {
      
      NSString *imagePath = [imagePaths objectAtIndex:idx];
      if (![imagePath hasSuffix:@"/"])
        imagePath = [NSString stringWithFormat:@"%@/", imagePath];
      if ([mainBundlePath hasPrefix:imagePath]) {
        
        isRunningFromDiskImage = YES;
        break;
        
      }
    }
    
    // ? should we ask every time the user runs from a read-only disk image
    if (!isRunningFromDiskImage) {
      
      // we don't need to check this bundle path again
      [defaults setObject:mainBundlePath forKey:kLastLaunchedPathKey];
      
    } else {
      // we're running from a disk image
      
      [NSApp activateIgnoringOtherApps:YES];
      
      NSString *displayName = [[NSFileManager defaultManager] displayNameAtPath:mainBundlePath];
      NSString *msg1template = NSLocalizedString(@"Would you like to copy %@ to your "
                                                 @"computer's Applications folder and "
                                                 @"run it from there?",
                                                 @"Copy app from disk image: title");
      NSString *msg1 = [NSString stringWithFormat:msg1template, displayName];
      NSString *msg2 = NSLocalizedString(@"%@ is currently running from the Disk Image, "
                                         @"and must be copied for full functionality. "
                                         @"Copying may replace an older version in the "
                                         @"Applications directory.",
                                         @"Copy app from disk image: message");
      NSString *btnOK = NSLocalizedString(@"Copy",
                                          @"Copy app from disk image: ok button");
      NSString *btnCancel = NSLocalizedString(@"Don't Copy",
                                              @"Copy app from disk image: cancel button");
      
      int result = NSRunAlertPanel(msg1, msg2, btnOK, btnCancel, NULL, displayName);
      if (result == NSAlertDefaultReturn) {
        // copy to /Applications and launch from there
        
        NSArray *appsPaths = NSSearchPathForDirectoriesInDomains(NSApplicationDirectory,
                                                                 NSLocalDomainMask,
                                                                 YES);
        if ([appsPaths count] > 0) {
          
          [self copyAndLaunchAppAtPath:mainBundlePath
                       toDirectoryPath:[appsPaths objectAtIndex:0]];  
          // calls exit(0) on successful copy/launch
        } else {
          NSLog(@"DiskImageUtilities: Cannot make applications folder path");
        }
      }
    }
  }
}

// copies an application from the given path to a new directory, if necessary
// authenticating as admin or killing a running process at that location
+ (void)copyAndLaunchAppAtPath:(NSString *)oldPath
               toDirectoryPath:(NSString *)newDirectory {
  
  NSFileManager *fileManager = [NSFileManager defaultManager];
  NSString *pathInApps = [newDirectory stringByAppendingPathComponent:[oldPath lastPathComponent]];
  BOOL isDir;
  BOOL dirPathExists = [fileManager fileExistsAtPath:pathInApps isDirectory:&isDir] && isDir;
  
  // We must authenticate as admin if we don't have write permission
  // in the /Apps directory, or if there's already an app there
  // with the same name and we don't have permission to write to it
  BOOL mustAuth = (![self canWriteToPath:newDirectory] 
                   || (dirPathExists && ![self canWriteToPath:pathInApps]));
  
  [self killAppAtPath:pathInApps];
  
  BOOL didCopy;
  if (mustAuth) {
    didCopy = [self doAuthorizedCopyFromPath:oldPath toPath:pathInApps];
  } else {
    didCopy = [fileManager copyPath:oldPath toPath:pathInApps handler:nil];
  }

  if (didCopy) {
    // launch the new copy and bail
    LSLaunchURLSpec spec;
    spec.appURL = (CFURLRef) [NSURL fileURLWithPath:pathInApps];
    spec.launchFlags = kLSLaunchNewInstance;
    spec.itemURLs = NULL;
    spec.passThruParams = NULL;
    spec.asyncRefCon = NULL;
    
    OSStatus err = LSOpenFromURLSpec(&spec, NULL); // NULL -> don't care about the launched URL
    if (err == noErr) {
      exit(0);
    } else {
      NSLog(@"DiskImageUtilities: Error %d launching \"%@\"", err, pathInApps);
    }
  } else {
    // copying to /Applications failed 
    NSLog(@"DiskImageUtilities: Error copying to \"%@\"", pathInApps);     
  }
}

// looks for an app running from the specified path, and calls KillProcess on it
+ (void)killAppAtPath:(NSString *)appPath {
  
  // get the FSRef for bundle of the the target app to kill
  FSRef targetFSRef;
  OSStatus err = FSPathMakeRef((const UInt8 *)[appPath fileSystemRepresentation],
                               &targetFSRef, nil);
  if (err == noErr) {
    
    // search for a PSN of a process with the bundle at that location, if any
    ProcessSerialNumber psn = { 0, kNoProcess };
    while (GetNextProcess(&psn) == noErr) {
      
      FSRef compareFSRef;
      if (GetProcessBundleLocation(&psn, &compareFSRef) == noErr
          && FSCompareFSRefs(&compareFSRef, &targetFSRef) == noErr) {
        
        // we found an app running from that path; kill it
        err = KillProcess(&psn);
        if (err != noErr) {
          NSLog(@"DiskImageUtilities: Could not kill process at %@, error %d",
                appPath, err);
        } 
      }
    }
  }
}

// canWriteToPath checks for permissions to write into the directory |path|
+ (BOOL)canWriteToPath:(NSString *)path {
  int stat = access([path fileSystemRepresentation], (W_OK | R_OK));
  return (stat == 0);
}

// doAuthorizedCopyFromPath does an authorized copy, getting admin rights
//
// NOTE: when running the task with admin privileges, this waits on any child 
// process, since AEWP doesn't tell us the child's pid.  This could be fooled 
// by any other child process that quits in the window between launch and 
// completion of our actual tool.
+ (BOOL)doAuthorizedCopyFromPath:(NSString *)src toPath:(NSString *)dest {
  // authorize
  AuthorizationFlags authFlags =  kAuthorizationFlagPreAuthorize 
                                | kAuthorizationFlagExtendRights
                                | kAuthorizationFlagInteractionAllowed;
  AuthorizationItem authItem = {kAuthorizationRightExecute, 0, nil, 0}; 
  AuthorizationRights authRights = {1, &authItem}; 
  SFAuthorization *authorization = [SFAuthorization authorizationWithFlags:authFlags
                                                                    rights:&authRights
                                                               environment:kAuthorizationEmptyEnvironment];
  
  // execute the copy
  const char taskPath[] = "/usr/bin/ditto";
  const char* arguments[] = { 
    "-rsrcFork",  // 0: copy resource forks; --rsrc requires 10.3
    NULL,  // 1: src path
    NULL,  // 2: dest path
    NULL 
  };
  arguments[1] = [src fileSystemRepresentation];
  arguments[2] = [dest fileSystemRepresentation];
  
  FILE **kNoPipe = nil;
  OSStatus status = AuthorizationExecuteWithPrivileges([authorization authorizationRef],
                                                       taskPath,
                                                       kAuthorizationFlagDefaults,
                                                       (char *const *)arguments,
                                                       kNoPipe);
  if (status == errAuthorizationSuccess) {
    int wait_status;
    int pid = wait(&wait_status);
    if (pid == -1 || !WIFEXITED(wait_status))	{
      status = -1;
    } else {
      status = WEXITSTATUS(wait_status);
    }
  }
  
  // deauthorize
  [authorization invalidateCredentials];
  
  return (status == 0);
}
@end
