from django.contrib import admin

from .models import (
    FrontKey, Items, ItemsPacks, ItemType, LotRequirements, Lots, LotType, SealedPacks, Secouristes, VerifItem, Verifs,
)


@admin.register(ItemType)
class ItemTypeAdmin(admin.ModelAdmin):
    list_display = ('type', 'name', 'min_quantity', 'perissable')


@admin.register(Items)
class ItemsAdmin(admin.ModelAdmin):
    list_display = ('iid', 'pack', 'location', 'status', 'last_seen')
    list_filter = ('status',)
    search_fields = ('iid',)


class LotRequirementsInline(admin.TabularInline):
    model = LotRequirements


@admin.register(LotType)
class LotTypeAdmin(admin.ModelAdmin):
    list_display = ('type', 'name', 'version')
    inlines = [LotRequirementsInline]


@admin.register(Lots)
class LotsAdmin(admin.ModelAdmin):
    list_display = ('id', 'name', 'lot_type', 'last_verif', 'active')


@admin.register(Secouristes)
class SecouristesAdmin(admin.ModelAdmin):
    list_display = ('matricule', 'nom', 'prenom', 'role', 'active', 'key_expires')


admin.site.register(ItemsPacks)
admin.site.register(SealedPacks)
admin.site.register(Verifs)
admin.site.register(VerifItem)


@admin.register(FrontKey)
class FrontKeyAdmin(admin.ModelAdmin):
    list_display = ('name', 'revoked', 'created', 'last_used', 'last_address')
    readonly_fields = ('name', 'key_hash', 'created', 'last_used', 'last_address')

    def has_add_permission(self, request):
        return False  # la cle n'est affichee qu'a la creation : `qrprotec-manage frontkey add NOM`
