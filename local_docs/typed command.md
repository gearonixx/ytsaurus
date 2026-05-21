
 ---

 TTypedCommand<TOptions> — это общая база любой driver-команды: парсит YSON-параметры в типизированный Options. TTabletCommandBase<TOptions> — её специализация именно для tablet-операций, которая сама
  наследуется от TTypedCommand и доливает три общих параметра, нужных всем mount/unmount/freeze/unfreeze/reshard: обязательный path и опциональные first_tablet_index / last_tablet_index (записываются прямо в
  Options).


---

![[Pasted image 20260520225556.png]]