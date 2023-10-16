# Generated by devtools/yamaker (pypi).

PY3_LIBRARY()

VERSION(3.0.1)

LICENSE(BSD-3-Clause)

PEERDIR(
    contrib/python/MarkupSafe
)

NO_LINT()

PY_SRCS(
    TOP_LEVEL
    wtforms/__init__.py
    wtforms/csrf/__init__.py
    wtforms/csrf/core.py
    wtforms/csrf/session.py
    wtforms/fields/__init__.py
    wtforms/fields/choices.py
    wtforms/fields/core.py
    wtforms/fields/datetime.py
    wtforms/fields/form.py
    wtforms/fields/list.py
    wtforms/fields/numeric.py
    wtforms/fields/simple.py
    wtforms/form.py
    wtforms/i18n.py
    wtforms/meta.py
    wtforms/utils.py
    wtforms/validators.py
    wtforms/widgets/__init__.py
    wtforms/widgets/core.py
)

RESOURCE_FILES(
    PREFIX contrib/python/WTForms/py3/
    .dist-info/METADATA
    .dist-info/top_level.txt
    wtforms/locale/README.md
    wtforms/locale/ar/LC_MESSAGES/wtforms.mo
    wtforms/locale/bg/LC_MESSAGES/wtforms.mo
    wtforms/locale/ca/LC_MESSAGES/wtforms.mo
    wtforms/locale/cs_CZ/LC_MESSAGES/wtforms.mo
    wtforms/locale/cy/LC_MESSAGES/wtforms.mo
    wtforms/locale/de/LC_MESSAGES/wtforms.mo
    wtforms/locale/de_CH/LC_MESSAGES/wtforms.mo
    wtforms/locale/el/LC_MESSAGES/wtforms.mo
    wtforms/locale/en/LC_MESSAGES/wtforms.mo
    wtforms/locale/es/LC_MESSAGES/wtforms.mo
    wtforms/locale/et/LC_MESSAGES/wtforms.mo
    wtforms/locale/fa/LC_MESSAGES/wtforms.mo
    wtforms/locale/fi/LC_MESSAGES/wtforms.mo
    wtforms/locale/fr/LC_MESSAGES/wtforms.mo
    wtforms/locale/he/LC_MESSAGES/wtforms.mo
    wtforms/locale/hu/LC_MESSAGES/wtforms.mo
    wtforms/locale/it/LC_MESSAGES/wtforms.mo
    wtforms/locale/ja/LC_MESSAGES/wtforms.mo
    wtforms/locale/ko/LC_MESSAGES/wtforms.mo
    wtforms/locale/nb/LC_MESSAGES/wtforms.mo
    wtforms/locale/nl/LC_MESSAGES/wtforms.mo
    wtforms/locale/pl/LC_MESSAGES/wtforms.mo
    wtforms/locale/pt/LC_MESSAGES/wtforms.mo
    wtforms/locale/ru/LC_MESSAGES/wtforms.mo
    wtforms/locale/sk/LC_MESSAGES/wtforms.mo
    wtforms/locale/sv/LC_MESSAGES/wtforms.mo
    wtforms/locale/tr/LC_MESSAGES/wtforms.mo
    wtforms/locale/uk/LC_MESSAGES/wtforms.mo
    wtforms/locale/wtforms.pot
    wtforms/locale/zh/LC_MESSAGES/wtforms.mo
    wtforms/locale/zh_TW/LC_MESSAGES/wtforms.mo
)

END()