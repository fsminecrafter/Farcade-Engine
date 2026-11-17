-- A script of the export demo: turns its object and reports on the serial line.
-- @prop speed number = 90 range 0 360
function on_start(self)
  print("FE GAME start " .. self.object.name)
  self.n = 0
  self.angle = 0
end

function on_update(self, dt)
  self.n = self.n + 1
  self.angle = self.angle + self.speed * dt
  self.object.rotation = fe.vec3(20, self.angle, 0)
  if self.n == 15 then print("FE GAME spin " .. self.n) end
end
