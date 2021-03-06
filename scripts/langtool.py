#!/usr/bin/python
#!/opt/local/bin/python2.7

# source -> .po
# po -> .lua

import sys, os
import getopt
import subprocess
import glob, re
import codecs
import vdf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(sys.argv[0])))

sys.path.append(os.path.join(ROOT, "server", "sync"))
from slpp import slpp as lua

import polib

all_languages = [ "de", "fr", "ru", "nl", "pl", "sv", "ko", "ja", "zh" ]
steam_languages = {"ru":"russian", "en":"english", "de":"german", "fr":"french", "pl":"polish", "sv":"swedish",
                   "ko":"korean", "ja":"japanese", "zh":"chinese"}
languages = all_languages
luafiles = ["tips", "popups", "messages", "tutorial"] # text.lua is special
txtfiles = ["ships"]

imprt = False
export = False


def mkdir_p(path):
    try:
        os.makedirs(path)
    except OSError as exc: # Python >2.5
        import errno
        if exc.errno == errno.EEXIST and os.path.isdir(path):
            pass
        else: raise

        
def header(luaf):
    return ("\nReassembly data/%s localization file\n" % os.path.basename(luaf)) + """
Copyright (C) 2015 Arthur Danskin
Content-Type: text/plain; charset=utf-8
"""


def txt2po(txtf, pof):
    pofil = polib.POFile()
    for line in open(txtf):
        pofil.append(polib.POEntry(msgid=line.strip(), tcomment="this is the name of a ship"))
    pofil.header = header(txtf)
    pofil.save(pof)
    print "wrote " + pof

    
def lua2po(luaf, pof):
    with open(luaf) as fil:
        luadata = fil.read()
    newpo = polib.POFile()
    if "web_header" in luadata:
        luadata = luadata[:luadata.index("web_header")]
    for st in re.findall('"[^"]+"', luadata):
        st = st[1:-1].replace("\\\n", "")
        if len(st) > 2:
            newpo.append(polib.POEntry(msgid=st))

    if os.path.exists(pof):
        pofil = polib.pofile(pofile=pof)
        pofil.merge(newpo)
    else:
        pofil = newpo
    pofil.header = header(luaf)
    pofil.save(pof)


def load_achievements():
    return vdf.load(open(os.path.join(ROOT, "lang", "329130_loc_all.vdf")))["lang"]["english"]["Tokens"]


def achievements_vdf2po(pof):
    if os.path.exists(pof):
        return
    pofil = polib.POFile()
    for key, val in load_achievements().iteritems():
        pofil.append(polib.POEntry(msgid=val, tcomment=key))
        pofil.header = header("achievements")
    pofil.save(pof)
    print "wrote " + pof


def achievements_po2vdf(vdff, langs):
    english = load_achievements().items()
    dat = {}
    for lang in langs:
        trans = dict((pe.msgid, pe.msgstr) for pe in polib.pofile(os.path.join(ROOT, "lang", lang, "achievements.po")))
        dat[steam_languages[lang]] = {"Tokens":dict((k, trans[v]) for k, v in english)}
    vdf.dump({"lang":dat}, codecs.open(vdff, 'w', 'utf-8'), pretty=True)
    print "wrote " + vdff
    
    
def po2lua_replace(outlua, baselua, pof):
    with open(baselua) as fil:
        dat = fil.read()
    count = 0
    for pe in polib.pofile(pof):
        if pe.msgstr:
            count += 1
            dat = dat.replace(pe.msgid, pe.msgstr, 1)
    if count == 0:
        return
    mkdir_p(os.path.dirname(outlua))
    with codecs.open(outlua, "w", "utf-8") as fil:
        fil.write(dat)

        
def escape(x):
    return x.replace('"', '\\"').replace("\n", "\\n")


def po2lua_simple(outlua, pofs):
    dct = {}
    print outlua, pofs
    for pof in pofs:
        if not os.path.exists(pof):
            continue
        for pe in polib.pofile(pof):
            if pe.msgstr:
                dct[pe.msgid] = pe.msgstr
    if not dct:
        return
    mkdir_p(os.path.dirname(outlua))
    fil = codecs.open(outlua, "w", "utf-8")
    fil.write("{" + ",\n".join('"%s" = "%s"' % (escape(k), escape(v)) for k, v in sorted(dct.items()) if k != v) + "}")
    fil.close()


def po2txt(outtxt, pof):
    fil = codecs.open(outtxt, "w", "utf-8")
    for pe in polib.pofile(pof):
        if pe.msgstr:
            fil.write(pe.tcomment + "\n")
            fil.write(pe.msgstr + "\n\n")
    fil.close()
    
    
def printhelp():
    print """%s
-i : import .po files to game format
-e : export .po files from game source
-l <lang> : select language (e.g. en_US)
-h : print this message""" % sys.argv[0]
    exit(0)

    
opts, cargs = getopt.getopt(sys.argv[1:], "ieh")
for (opt, val) in opts:
    if opt == "-h":
        printhelp()
    elif opt == "-i":
        imprt = True
    elif opt == "-e":
        export = True

if cargs:
    languages = cargs

if export:
    sources = []
    for d in ("game", "core"):
        for e in (".cpp", ".h"):
            sources.extend(glob.glob(os.path.join(ROOT, d, "*%s" % e)))
    for l in ["factions.lua"]:
        sources.append(os.path.join(ROOT, "data", l))
    print "exporting from %d files" % len(sources)
    pofils = []
    for lang in languages:
        lroot = os.path.join(ROOT, "lang", lang)
        outpt = os.path.join(lroot, "text.po")
        pofils.append(outpt)
        mkdir_p(lroot)
        args = ["xgettext", "--keyword=_", "--output=" + outpt, "--omit-header",
                "--copyright-holder=2015 Arthur Danskin"]
        if os.path.exists(outpt):
            args.append("--join-existing")
        ret = subprocess.call(args + sources)
        # ret = 0
        if ret != 0:
            print "%s\nreturned exit code %d" % (" ".join(args + sources), ret)
            exit(ret)
        for fil in luafiles:
            pof = os.path.join(lroot, fil + ".po")
            pofils.append(pof)
            lua2po(os.path.join(ROOT, "data", fil + ".lua"), pof)
        print "wrote %d lua .po files for %s" % (len(luafiles), lang)
        for fil in txtfiles:
            pof = os.path.join(lroot, fil + ".po")
            pofils.append(pof)
            txt2po(os.path.join(ROOT, "lang", fil + ".txt"), pof)
        achievements_vdf2po(os.path.join(lroot, "achievements.po"))
    subprocess.call(["unix2dos", "-q"] + pofils)
    print "export complete"
elif imprt:
    for lang in languages:
        dbase = os.path.join(ROOT, "data")
        droot = os.path.join(dbase, "lang", lang)
        lroot = os.path.join(ROOT, "lang", lang)
        po2lua_simple(os.path.join(droot, "text.lua"),
                      [os.path.join(lroot, x + ".po") for x in txtfiles + ["text"]])
        for fil in luafiles:
            po2lua_replace(os.path.join(droot, fil + ".lua"),
                           os.path.join(dbase, fil + ".lua"),
                           os.path.join(lroot, fil + ".po"))
    achievements_po2vdf(os.path.expanduser("~/Downloads/achievements.vdf"), languages)
    po2txt(os.path.expanduser("~/Downloads/store.txt"), os.path.join(lroot, "store.po"))
    print "import complete"
        

else:
    printhelp()        
