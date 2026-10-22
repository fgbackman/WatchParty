#!/usr/bin/env python3
"""Writes ios/WatchParty.xcodeproj/project.pbxproj (no Xcode needed to regenerate it).

The iPhone app is plain C: ios/wp_ios.c (UIKit through the Objective-C runtime) on
top of the same engine as the Mac app in ../src. Usage: python3 ios/make_project.py
"""
import hashlib, os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "WatchParty.xcodeproj", "project.pbxproj")

SHARED = ["wp_room.c", "wp_net.c", "wp_lobby.c", "wp_ec.c", "wp_search.c", "wp_search_parse.c",
          "wp_json.c", "wp_util.c", "wp_fancy.c"]
SHARED_H = ["objc_glue.h", "wp_room.h", "wp_net.h", "wp_lobby.h", "wp_search.h", "wp_json.h", "wp_util.h",
            "wp_fancy.h", "wp_fancy_table.h"]
RES = ["player.html", "ate.png", "ate@2x.png"]

def uid(*parts):
    return hashlib.md5("/".join(parts).encode()).hexdigest()[:24].upper()

files = {}          # key -> (id, name, path, type, sourceTree)
def fref(key, name, path, ftype, tree="<group>"):
    files[key] = (uid("file", key), name, path, ftype, tree)
    return files[key][0]

for f in SHARED: fref("src/" + f, f, f, "sourcecode.c.c")
for f in SHARED_H: fref("src/" + f, f, f, "sourcecode.c.h")
for f in RES: fref("res/" + f, f, f, "text.html" if f.endswith(".html") else "image.png")
fref("ios/wp_ios.c", "wp_ios.c", "wp_ios.c", "sourcecode.c.c")
fref("ios/Info.plist", "Info.plist", "Info.plist", "text.plist.xml")
fref("ios/Assets.xcassets", "Assets.xcassets", "Assets.xcassets", "folder.assetcatalog")
PRODUCT = uid("product")

sources = ["src/" + f for f in SHARED] + ["ios/wp_ios.c"]
resources = ["res/" + f for f in RES] + ["ios/Assets.xcassets"]
build = {k: uid("build", k) for k in sources + resources}

G_MAIN, G_SHARED, G_IOS, G_RES, G_PROD = (uid("group", n) for n in ("main", "shared", "ios", "res", "products"))
TARGET, PROJECT = uid("target"), uid("project")
P_SRC, P_RES, P_FW = uid("phase", "src"), uid("phase", "res"), uid("phase", "fw")
CL_PROJ, CL_TGT = uid("cl", "proj"), uid("cl", "tgt")
CF = {k: uid("cfg", k) for k in ("pd", "pr", "td", "tr")}

def q(s):
    return '"%s"' % s if any(c in s for c in ' -+@$/(),=<>') or s == "" else s

L = []
w = L.append
w("// !$*UTF8*$!\n{\n\tarchiveVersion = 1;\n\tclasses = {\n\t};\n\tobjectVersion = 56;\n\tobjects = {\n")

w("\n/* Begin PBXBuildFile section */\n")
for k in sources:
    w("\t\t%s /* %s in Sources */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };\n" % (build[k], files[k][1], files[k][0], files[k][1]))
for k in resources:
    w("\t\t%s /* %s in Resources */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };\n" % (build[k], files[k][1], files[k][0], files[k][1]))
w("/* End PBXBuildFile section */\n")

w("\n/* Begin PBXFileReference section */\n")
for k, (i, name, path, ftype, tree) in sorted(files.items(), key=lambda kv: kv[1][0]):
    w("\t\t%s /* %s */ = {isa = PBXFileReference; lastKnownFileType = %s; path = %s; sourceTree = %s; };\n"
      % (i, name, ftype, q(path), q(tree)))
w("\t\t%s /* WatchParty.app */ = {isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = WatchParty.app; sourceTree = BUILT_PRODUCTS_DIR; };\n" % PRODUCT)
w("/* End PBXFileReference section */\n")

w("\n/* Begin PBXFrameworksBuildPhase section */\n")
w("\t\t%s /* Frameworks */ = {\n\t\t\tisa = PBXFrameworksBuildPhase;\n\t\t\tbuildActionMask = 2147483647;\n\t\t\tfiles = (\n\t\t\t);\n\t\t\trunOnlyForDeploymentPostprocessing = 0;\n\t\t};\n" % P_FW)
w("/* End PBXFrameworksBuildPhase section */\n")

def group(gid, name, children, path=None):
    w("\t\t%s /* %s */ = {\n\t\t\tisa = PBXGroup;\n\t\t\tchildren = (\n" % (gid, name))
    for c, cn in children: w("\t\t\t\t%s /* %s */,\n" % (c, cn))
    w("\t\t\t);\n")
    if path: w("\t\t\tpath = %s;\n" % q(path))
    elif name: w("\t\t\tname = %s;\n" % q(name))
    w("\t\t\tsourceTree = \"<group>\";\n\t\t};\n")

w("\n/* Begin PBXGroup section */\n")
group(G_MAIN, "", [(G_IOS, "iPhone"), (G_SHARED, "Shared engine"), (G_RES, "Resources"), (G_PROD, "Products")])
group(G_IOS, "iPhone", [(files[k][0], files[k][1]) for k in ("ios/wp_ios.c", "ios/Info.plist", "ios/Assets.xcassets")])
w("\t\t%s /* Shared engine */ = {\n\t\t\tisa = PBXGroup;\n\t\t\tchildren = (\n" % G_SHARED)
for f in SHARED + SHARED_H: w("\t\t\t\t%s /* %s */,\n" % (files["src/" + f][0], f))
w("\t\t\t);\n\t\t\tname = \"Shared engine\";\n\t\t\tpath = ../src;\n\t\t\tsourceTree = \"<group>\";\n\t\t};\n")
w("\t\t%s /* Resources */ = {\n\t\t\tisa = PBXGroup;\n\t\t\tchildren = (\n" % G_RES)
for f in RES: w("\t\t\t\t%s /* %s */,\n" % (files["res/" + f][0], f))
w("\t\t\t);\n\t\t\tname = Resources;\n\t\t\tpath = ../Resources;\n\t\t\tsourceTree = \"<group>\";\n\t\t};\n")
w("\t\t%s /* Products */ = {\n\t\t\tisa = PBXGroup;\n\t\t\tchildren = (\n\t\t\t\t%s /* WatchParty.app */,\n\t\t\t);\n\t\t\tname = Products;\n\t\t\tsourceTree = \"<group>\";\n\t\t};\n" % (G_PROD, PRODUCT))
w("/* End PBXGroup section */\n")
# the iPhone group lives in this folder: no path needed (files are relative to the project dir)

w("\n/* Begin PBXNativeTarget section */\n")
w("""\t\t%s /* WatchParty */ = {
\t\t\tisa = PBXNativeTarget;
\t\t\tbuildConfigurationList = %s /* Build configuration list for PBXNativeTarget "WatchParty" */;
\t\t\tbuildPhases = (
\t\t\t\t%s /* Sources */,
\t\t\t\t%s /* Frameworks */,
\t\t\t\t%s /* Resources */,
\t\t\t);
\t\t\tbuildRules = (
\t\t\t);
\t\t\tdependencies = (
\t\t\t);
\t\t\tname = WatchParty;
\t\t\tproductName = WatchParty;
\t\t\tproductReference = %s /* WatchParty.app */;
\t\t\tproductType = "com.apple.product-type.application";
\t\t};
""" % (TARGET, CL_TGT, P_SRC, P_FW, P_RES, PRODUCT))
w("/* End PBXNativeTarget section */\n")

w("\n/* Begin PBXProject section */\n")
w("""\t\t%s /* Project object */ = {
\t\t\tisa = PBXProject;
\t\t\tattributes = {
\t\t\t\tBuildIndependentTargetsInParallel = 1;
\t\t\t\tLastUpgradeCheck = 1600;
\t\t\t\tTargetAttributes = {
\t\t\t\t\t%s = {
\t\t\t\t\t\tCreatedOnToolsVersion = 16.0;
\t\t\t\t\t};
\t\t\t\t};
\t\t\t};
\t\t\tbuildConfigurationList = %s /* Build configuration list for PBXProject "WatchParty" */;
\t\t\tcompatibilityVersion = "Xcode 14.0";
\t\t\tdevelopmentRegion = en;
\t\t\thasScannedForEncodings = 0;
\t\t\tknownRegions = (
\t\t\t\ten,
\t\t\t\tBase,
\t\t\t);
\t\t\tmainGroup = %s;
\t\t\tproductRefGroup = %s /* Products */;
\t\t\tprojectDirPath = "";
\t\t\tprojectRoot = "";
\t\t\ttargets = (
\t\t\t\t%s /* WatchParty */,
\t\t\t);
\t\t};
""" % (PROJECT, TARGET, CL_PROJ, G_MAIN, G_PROD, TARGET))
w("/* End PBXProject section */\n")

def phase(pid, isa, name, keys, kind):
    w("\t\t%s /* %s */ = {\n\t\t\tisa = %s;\n\t\t\tbuildActionMask = 2147483647;\n\t\t\tfiles = (\n" % (pid, name, isa))
    for k in keys: w("\t\t\t\t%s /* %s in %s */,\n" % (build[k], files[k][1], kind))
    w("\t\t\t);\n\t\t\trunOnlyForDeploymentPostprocessing = 0;\n\t\t};\n")

w("\n/* Begin PBXResourcesBuildPhase section */\n")
phase(P_RES, "PBXResourcesBuildPhase", "Resources", resources, "Resources")
w("/* End PBXResourcesBuildPhase section */\n")
w("\n/* Begin PBXSourcesBuildPhase section */\n")
phase(P_SRC, "PBXSourcesBuildPhase", "Sources", sources, "Sources")
w("/* End PBXSourcesBuildPhase section */\n")

def settings(d):
    out = ""
    for k in sorted(d):
        v = d[k]
        if isinstance(v, list):
            out += "\t\t\t\t%s = (\n" % k + "".join("\t\t\t\t\t%s,\n" % q(x) for x in v) + "\t\t\t\t);\n"
        else:
            out += "\t\t\t\t%s = %s;\n" % (k, q(str(v)))
    return out

proj_common = {
    "ALWAYS_SEARCH_USER_PATHS": "NO", "CLANG_ENABLE_OBJC_ARC": "NO", "GCC_C_LANGUAGE_STANDARD": "gnu11",
    "IPHONEOS_DEPLOYMENT_TARGET": "15.0", "SDKROOT": "iphoneos", "CLANG_WARN_UNREACHABLE_CODE": "YES",
    "GCC_WARN_UNUSED_VARIABLE": "YES", "ENABLE_STRICT_OBJC_MSGSEND": "NO",
}
proj_debug = dict(proj_common, DEBUG_INFORMATION_FORMAT="dwarf", GCC_OPTIMIZATION_LEVEL="0", ONLY_ACTIVE_ARCH="YES",
                  ENABLE_TESTABILITY="YES", GCC_PREPROCESSOR_DEFINITIONS=["DEBUG=1", "$(inherited)"])
proj_release = dict(proj_common, DEBUG_INFORMATION_FORMAT="dwarf-with-dsym", GCC_OPTIMIZATION_LEVEL="s",
                    VALIDATE_PRODUCT="YES")
tgt = {
    "ASSETCATALOG_COMPILER_APPICON_NAME": "AppIcon", "CODE_SIGN_STYLE": "Automatic", "CURRENT_PROJECT_VERSION": "2",
    "DEVELOPMENT_TEAM": "", "GENERATE_INFOPLIST_FILE": "NO", "HEADER_SEARCH_PATHS": ["$(SRCROOT)/../src"],
    "INFOPLIST_FILE": "Info.plist", "LD_RUNPATH_SEARCH_PATHS": ["$(inherited)", "@executable_path/Frameworks"],
    "MARKETING_VERSION": "1.1", "OTHER_CFLAGS": ["-fblocks"],
    "OTHER_LDFLAGS": ["-framework", "UIKit", "-framework", "WebKit", "-framework", "Network", "-framework", "Security",
                      "-framework", "Foundation", "-framework", "CoreFoundation", "-framework", "CoreGraphics", "-lobjc"],
    "PRODUCT_BUNDLE_IDENTIFIER": "com.fredbackman.watchparty.ios", "PRODUCT_NAME": "$(TARGET_NAME)",
    "TARGETED_DEVICE_FAMILY": "1,2", "WARNING_CFLAGS": ["-Wno-unused-parameter", "-Wno-unused-function"],
}

w("\n/* Begin XCBuildConfiguration section */\n")
for cid, name, d in ((CF["pd"], "Debug", proj_debug), (CF["pr"], "Release", proj_release), (CF["td"], "Debug", tgt), (CF["tr"], "Release", tgt)):
    w("\t\t%s /* %s */ = {\n\t\t\tisa = XCBuildConfiguration;\n\t\t\tbuildSettings = {\n%s\t\t\t};\n\t\t\tname = %s;\n\t\t};\n" % (cid, name, settings(d), name))
w("/* End XCBuildConfiguration section */\n")

w("\n/* Begin XCConfigurationList section */\n")
for lid, what, d, r in ((CL_PROJ, 'PBXProject "WatchParty"', CF["pd"], CF["pr"]), (CL_TGT, 'PBXNativeTarget "WatchParty"', CF["td"], CF["tr"])):
    w("\t\t%s /* Build configuration list for %s */ = {\n\t\t\tisa = XCConfigurationList;\n\t\t\tbuildConfigurations = (\n\t\t\t\t%s /* Debug */,\n\t\t\t\t%s /* Release */,\n\t\t\t);\n\t\t\tdefaultConfigurationIsVisible = 0;\n\t\t\tdefaultConfigurationName = Release;\n\t\t};\n" % (lid, what, d, r))
w("/* End XCConfigurationList section */\n")

w("\t};\n\trootObject = %s /* Project object */;\n}\n" % PROJECT)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
open(OUT, "w").write("".join(L))

# a shared scheme, so Xcode opens straight to a runnable target
sd = os.path.join(HERE, "WatchParty.xcodeproj", "xcshareddata", "xcschemes")
os.makedirs(sd, exist_ok=True)
ref = '''<BuildableReference BuildableIdentifier = "primary" BlueprintIdentifier = "%s" BuildableName = "WatchParty.app" BlueprintName = "WatchParty" ReferencedContainer = "container:WatchParty.xcodeproj"></BuildableReference>''' % TARGET
open(os.path.join(sd, "WatchParty.xcscheme"), "w").write('''<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion = "1600" version = "1.7">
   <BuildAction parallelizeBuildables = "YES" buildImplicitDependencies = "YES">
      <BuildActionEntries>
         <BuildActionEntry buildForTesting = "YES" buildForRunning = "YES" buildForProfiling = "YES" buildForArchiving = "YES" buildForAnalyzing = "YES">
            %s
         </BuildActionEntry>
      </BuildActionEntries>
   </BuildAction>
   <TestAction buildConfiguration = "Debug" selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB" shouldUseLaunchSchemeArgsEnv = "YES">
   </TestAction>
   <LaunchAction buildConfiguration = "Debug" selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB" launchStyle = "0" useCustomWorkingDirectory = "NO" ignoresPersistentStateOnLaunch = "NO" debugDocumentVersioning = "YES" debugServiceExtension = "internal" allowLocationSimulation = "YES">
      <BuildableProductRunnable runnableDebuggingMode = "0">
         %s
      </BuildableProductRunnable>
   </LaunchAction>
   <ProfileAction buildConfiguration = "Release" shouldUseLaunchSchemeArgsEnv = "YES" savedToolIdentifier = "" useCustomWorkingDirectory = "NO" debugDocumentVersioning = "YES">
   </ProfileAction>
   <AnalyzeAction buildConfiguration = "Debug">
   </AnalyzeAction>
   <ArchiveAction buildConfiguration = "Release" revealArchiveInOrganizer = "YES">
   </ArchiveAction>
</Scheme>
''' % (ref, ref))
print("wrote", OUT)
