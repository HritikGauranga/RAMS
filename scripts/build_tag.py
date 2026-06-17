import time
Import("env")

# Force a unique firmware tag per build invocation so serial lock
# can be reopened after each fresh firmware upload.
tag = str(int(time.time()))
env.Append(CPPDEFINES=[("FW_BUILD_TAG", '\\"%s\\"' % tag)])
