# QRProtec : consignes pour Claude

## Python : tout le code est typé

Tout le code Python du dépôt (back Django `database/`, `packaging/`, `tools/bench/qrprotec-bench`, tests compris) est
entièrement annoté. C'est la règle pour tout nouveau code et pour tout code modifié :

- Chaque fonction et méthode annote tous ses paramètres et son type de retour (`-> None` compris), `*args` et
  `**kwargs` aussi (`**kwargs: Any`). Les fonctions imbriquées et les vues aussi (`request: Request`, `-> Response`
  pour une vue DRF ; `HttpRequest` / `HttpResponse` pour une vue Django simple).
- Les variables dont mypy ne devine pas le type sont annotées : conteneurs vides (`seen: set[str] = set()`,
  `counts: defaultdict[str, int] = defaultdict(int)`), attributs initialisés à `None` (`self.socket: WebSocket | None
  = None`).
- Syntaxe moderne : `X | None`, `list[str]`, `dict[str, Any]`, `collections.abc.Callable[[Items], bool]`, jamais
  `Optional`, `List`, `Dict`. Chaque module commence (après sa docstring) par `from __future__ import annotations`.
- Les imports qui ne servent qu'aux annotations vont sous `if TYPE_CHECKING:` (pas d'import circulaire, rien de plus
  chargé au démarrage).
- Types précis plutôt que `Any` ; `Any` reste admis pour les données JSON (`dict[str, Any]` d'une réponse ou de
  `request.data`) et les valeurs vraiment dynamiques.
- Le typage ne change jamais le comportement : pas de code ajouté ou modifié juste pour faire plaisir à mypy sans
  le dire.

Vérification, depuis la racine du dépôt (configuration dans `pyproject.toml`, `[tool.mypy]`) :

```sh
pip install mypy django-stubs djangorestframework-stubs
QRPROTEC_DEBUG=1 mypy
```

mypy doit finir sans erreur : il refuse toute fonction non annotée et tout type invalide ou inconnu. Quelques codes
d'erreur sont encore désactivés (`disable_error_code`) pour le code existant ; on peut les réactiver un par un en
corrigeant le code, jamais en ajouter d'autres pour faire passer du code neuf.

Tests du back : `cd database && QRPROTEC_DEBUG=1 python manage.py test --parallel 8`.
