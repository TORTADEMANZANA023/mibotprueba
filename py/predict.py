import network
from model import ChessCoachModel
import numpy

model = ChessCoachModel()
model.build()

def predict(image):
  image = image.reshape((1, 12, 8, 8)) # TODO: Only one at a time right now, need to parallelize across MCTSs
  value, policy = model.model.predict_on_batch(image)

  value = numpy.reshape(network.map_11_to_01(value), (1,1))
  policy = numpy.reshape(policy, (73, 8, 8)) # TODO: Only one at a time right now, need to parallelize across MCTSs
  print("Policy[0][1][2]:", policy[0][1][2])
  print("Policy[3][4][5]:", policy[3][4][5])

  return value, policy